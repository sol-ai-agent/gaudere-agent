#include "OpenAIResponsesProvider.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t max_openai_output_bytes = 256 * 1024;
constexpr std::uint64_t max_openai_output_tokens = 1024;
constexpr std::uint64_t max_openai_response_bytes = 1024 * 1024;
constexpr std::uint64_t response_envelope_bytes = 64 * 1024;
constexpr std::string_view production_endpoint = "https://api.openai.com/v1/responses";
constexpr std::string_view usage_content_type =
    "application/vnd.gaudere.provider-usage+json";

struct NormalizedUsage {
    bool present = false;
    bool valid = true;
    std::string metadata;
};

bool text_plain(const std::string_view content_type)
{
    constexpr std::string_view prefix = "text/plain";
    if (content_type.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        const auto actual = static_cast<unsigned char>(content_type[index]);
        if (static_cast<char>(std::tolower(actual)) != prefix[index]) {
            return false;
        }
    }
    return content_type.size() == prefix.size()
        || content_type[prefix.size()] == ';';
}

bool safe_bearer_secret(const std::string_view secret) noexcept
{
    for (const unsigned char character : secret) {
        if (character < 0x21 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> json_string(const Json& object, const char* key)
{
    if (!object.is_object()) {
        return std::nullopt;
    }
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

bool nonnegative_integer(const Json& value, std::uint64_t& output)
{
    try {
        if (value.is_number_unsigned()) {
            output = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const auto signed_value = value.get<std::int64_t>();
            if (signed_value < 0) {
                return false;
            }
            output = static_cast<std::uint64_t>(signed_value);
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool required_token_count(const Json& object,
                          const char* key,
                          std::uint64_t& output)
{
    if (!object.is_object()) {
        return false;
    }
    const auto found = object.find(key);
    return found != object.end() && nonnegative_integer(*found, output);
}

bool optional_detail_token_count(const Json& usage,
                                 const char* details_key,
                                 const char* token_key,
                                 std::uint64_t& output)
{
    output = 0;
    const auto details = usage.find(details_key);
    if (details == usage.end() || details->is_null()) {
        return true;
    }
    if (!details->is_object()) {
        return false;
    }
    const auto found = details->find(token_key);
    if (found == details->end() || found->is_null()) {
        return true;
    }
    return nonnegative_integer(*found, output);
}

NormalizedUsage normalized_usage(const Json& document,
                                 const std::string& configured_model)
{
    const auto usage = document.find("usage");
    if (usage == document.end() || usage->is_null()) {
        return NormalizedUsage{};
    }
    NormalizedUsage result;
    result.present = true;
    if (!usage->is_object()) {
        result.valid = false;
        return result;
    }

    std::uint64_t input_tokens = 0;
    std::uint64_t output_tokens = 0;
    std::uint64_t total_tokens = 0;
    std::uint64_t cached_input_tokens = 0;
    std::uint64_t cache_write_input_tokens = 0;
    std::uint64_t reasoning_tokens = 0;
    if (!required_token_count(*usage, "input_tokens", input_tokens)
        || !required_token_count(*usage, "output_tokens", output_tokens)
        || !required_token_count(*usage, "total_tokens", total_tokens)
        || !optional_detail_token_count(*usage, "input_tokens_details",
                                        "cached_tokens", cached_input_tokens)
        || !optional_detail_token_count(*usage, "input_tokens_details",
                                        "cache_write_tokens", cache_write_input_tokens)
        || !optional_detail_token_count(*usage, "output_tokens_details",
                                        "reasoning_tokens", reasoning_tokens)) {
        result.valid = false;
        return result;
    }

    const Json normalized = {
        {"schema", "gaudere.provider_usage.v1"},
        {"provider", "openai"},
        {"model", configured_model},
        {"input_tokens", input_tokens},
        {"cached_input_tokens", cached_input_tokens},
        {"cache_write_input_tokens", cache_write_input_tokens},
        {"output_tokens", output_tokens},
        {"reasoning_tokens", reasoning_tokens},
        {"total_tokens", total_tokens}
    };
    result.metadata = normalized.dump();
    return result;
}

std::string incomplete_reason(const Json& document)
{
    if (!document.is_object()) {
        return "OpenAI response is incomplete";
    }
    const auto details = document.find("incomplete_details");
    if (details != document.end() && details->is_object()) {
        if (const auto reason = json_string(*details, "reason")) {
            return *reason;
        }
    }
    return "OpenAI response is incomplete";
}

std::uint64_t fnv1a(const std::string_view value, std::uint64_t hash) noexcept
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::string hex64(const std::uint64_t value)
{
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

} // namespace

OpenAIResponsesProvider::OpenAIResponsesProvider(
    HttpTransport& transport,
    SecretSource& secrets,
    std::string model,
    std::string secret_name,
    std::string endpoint)
    : transport_(transport),
      secrets_(secrets),
      model_(std::move(model)),
      secret_name_(std::move(secret_name)),
      endpoint_(std::move(endpoint))
{
    if (model_.empty()) {
        throw std::invalid_argument("OpenAI model must not be empty");
    }
    if (secret_name_.empty()) {
        throw std::invalid_argument("OpenAI secret name must not be empty");
    }
    if (endpoint_.rfind("https://", 0) != 0) {
        throw std::invalid_argument("OpenAI endpoint must use https://");
    }
}

ProviderResult OpenAIResponsesProvider::rejected(
    std::string code,
    std::string message,
    std::string metadata_content_type,
    std::string metadata)
{
    return ProviderResult{ProviderOutcome::rejected, {}, {},
                          std::move(code), std::move(message),
                          std::move(metadata_content_type), std::move(metadata)};
}

ProviderResult OpenAIResponsesProvider::unknown(std::string code,
                                                std::string message)
{
    return ProviderResult{ProviderOutcome::effect_unknown, {}, {},
                          std::move(code), std::move(message)};
}

std::uint64_t OpenAIResponsesProvider::response_body_limit(
    const std::uint64_t max_output_bytes) noexcept
{
    if (max_output_bytes >= (max_openai_response_bytes - response_envelope_bytes) / 4) {
        return max_openai_response_bytes;
    }
    return std::min(max_openai_response_bytes,
                    response_envelope_bytes + max_output_bytes * 4);
}

std::string OpenAIResponsesProvider::client_request_id(const std::string_view key)
{
    constexpr std::uint64_t offset1 = 14695981039346656037ULL;
    constexpr std::uint64_t offset2 = 1099511628211ULL ^ 0x9e3779b97f4a7c15ULL;
    return "gaudere-" + hex64(fnv1a(key, offset1))
        + hex64(fnv1a(key, offset2));
}

ProviderResult OpenAIResponsesProvider::invoke(const ProviderRequest& request)
{
    if (!text_plain(request.content_type)) {
        return rejected("openai_unsupported_input_type",
                        "initial OpenAI adapter accepts text/plain input only");
    }
    if (request.max_runtime.count() <= 0) {
        return rejected("openai_invalid_runtime_limit",
                        "OpenAI request runtime limit must be positive");
    }
    if (request.max_output_bytes == 0
        || request.max_output_bytes > max_openai_output_bytes) {
        return rejected("openai_unsupported_output_limit",
                        "OpenAI output byte limit must be between 1 and 262144 bytes");
    }

    std::optional<SecretValue> secret;
    try {
        secret = secrets_.load(secret_name_);
    } catch (const std::exception& error) {
        return rejected("openai_secret_error", error.what());
    }
    if (!secret) {
        return rejected("openai_secret_missing", "OpenAI API secret is not available");
    }
    if (secret->empty()) {
        return rejected("openai_secret_empty", "OpenAI API secret is empty");
    }
    if (!safe_bearer_secret(secret->view())) {
        return rejected(
            "openai_secret_invalid",
            "OpenAI API secret must contain printable ASCII without whitespace or line breaks");
    }

    Json payload = {
        {"model", model_},
        {"input", request.input},
        {"max_output_tokens", max_openai_output_tokens},
        {"store", false},
        {"stream", false}
    };

    HttpRequest http_request;
    http_request.method = "POST";
    http_request.url = endpoint_;
    http_request.headers = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"X-Client-Request-Id", client_request_id(request.idempotency_key)}
    };
    http_request.body = payload.dump();
    http_request.timeout = request.max_runtime;
    http_request.max_response_bytes = response_body_limit(request.max_output_bytes);

    HttpTransportResult transport_result;
    try {
        transport_result = transport_.perform(
            http_request,
            HttpSensitiveHeader{"Authorization", "Bearer ", secret->view()});
    } catch (const std::exception& error) {
        return unknown("openai_transport_exception", error.what());
    } catch (...) {
        return unknown("openai_transport_exception",
                       "HTTP transport threw a non-standard exception");
    }

    if (transport_result.outcome == HttpTransportOutcome::effect_unknown
        || !transport_result.response) {
        return unknown(
            transport_result.failure_code.empty()
                ? "openai_transport_unknown"
                : std::move(transport_result.failure_code),
            transport_result.failure_message.empty()
                ? "OpenAI transport result is ambiguous"
                : std::move(transport_result.failure_message));
    }

    const auto& response = *transport_result.response;
    if (response.status < 200 || response.status >= 300) {
        // Provider-supplied error text is deliberately not persisted. Authentication
        // errors may echo masked fragments of the credential and other provider
        // messages may contain request data. The durable status code is sufficient
        // to classify a definite HTTP rejection; raw response text stays at the
        // transient transport boundary.
        return rejected("openai_http_" + std::to_string(response.status),
                        "OpenAI returned HTTP " + std::to_string(response.status));
    }

    Json document;
    try {
        document = Json::parse(response.body);
    } catch (const std::exception& error) {
        return rejected("openai_invalid_json_response", error.what());
    }
    if (!document.is_object()) {
        return rejected("openai_invalid_response",
                        "OpenAI success response is not a JSON object");
    }

    const auto status = json_string(document, "status");
    if (!status) {
        return rejected("openai_invalid_response",
                        "OpenAI response has no string status");
    }

    const auto usage = normalized_usage(document, model_);
    if (!usage.valid) {
        return rejected("openai_invalid_usage",
                        "OpenAI response contains malformed token usage");
    }
    const std::string metadata_content_type = usage.present
        ? std::string(usage_content_type)
        : std::string{};
    const auto reject_with_usage = [&](std::string code, std::string message) {
        return rejected(std::move(code), std::move(message),
                        metadata_content_type, usage.metadata);
    };

    if (*status == "incomplete") {
        return reject_with_usage("openai_incomplete", incomplete_reason(document));
    }
    if (*status == "failed") {
        // As with non-2xx responses, do not durably retain provider-supplied error
        // messages. They are not part of Gaudere's trusted diagnostic vocabulary.
        return reject_with_usage("openai_failed",
                                 "OpenAI response reported failed status");
    }
    if (*status != "completed") {
        return reject_with_usage("openai_unexpected_status",
                                 "unexpected OpenAI response status: " + *status);
    }

    const auto output = document.find("output");
    if (output == document.end() || !output->is_array()) {
        return reject_with_usage("openai_invalid_response",
                                 "completed OpenAI response has no output array");
    }

    std::string text;
    std::optional<std::string> refusal;
    for (const auto& item : *output) {
        if (!item.is_object()) {
            continue;
        }
        const auto type = json_string(item, "type");
        if (!type || *type != "message") {
            continue;
        }
        const auto content = item.find("content");
        if (content == item.end() || !content->is_array()) {
            continue;
        }
        for (const auto& part : *content) {
            if (!part.is_object()) {
                continue;
            }
            const auto part_type = json_string(part, "type");
            if (!part_type) {
                continue;
            }
            if (*part_type == "refusal") {
                if (const auto value = json_string(part, "refusal")) {
                    refusal = *value;
                }
                continue;
            }
            if (*part_type != "output_text") {
                continue;
            }
            const auto value = json_string(part, "text");
            if (!value) {
                return reject_with_usage("openai_invalid_response",
                                         "OpenAI output_text part has no text");
            }
            if (value->size() > request.max_output_bytes - text.size()) {
                return reject_with_usage("openai_output_too_large",
                                         "OpenAI text output exceeds the task byte limit");
            }
            text += *value;
        }
    }

    if (refusal) {
        return reject_with_usage("openai_refusal", *refusal);
    }
    if (text.empty()) {
        return reject_with_usage("openai_no_text_output",
                                 "completed OpenAI response contains no output_text");
    }

    if (!usage.present && endpoint_ == production_endpoint) {
        // A successful production model result without accounting cannot enter
        // durable state as success. The Action is still definite/confirmed and its
        // call permit remains consumed, but the Task fails closed instead of
        // silently losing cost observability. Synthetic/custom endpoints used by
        // offline tests are not required to emulate OpenAI billing telemetry.
        return rejected("openai_usage_missing",
                        "completed OpenAI response has no token usage");
    }

    return ProviderResult{ProviderOutcome::succeeded,
                          "text/plain; charset=utf-8",
                          std::move(text), {}, {},
                          metadata_content_type, usage.metadata};
}

} // namespace gaudere_agent
