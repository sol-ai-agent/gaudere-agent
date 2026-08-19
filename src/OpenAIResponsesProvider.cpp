#include "OpenAIResponsesProvider.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t max_openai_output_bytes = 256 * 1024;
constexpr std::uint64_t max_openai_response_bytes = 1024 * 1024;
constexpr std::uint64_t response_envelope_bytes = 64 * 1024;

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

std::optional<std::string> json_string(const Json& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

std::string provider_error_message(const Json& document,
                                   const std::string& fallback)
{
    const auto error = document.find("error");
    if (error != document.end() && error->is_object()) {
        if (const auto message = json_string(*error, "message")) {
            return *message;
        }
    }
    return fallback;
}

std::string incomplete_reason(const Json& document)
{
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
    if (endpoint_.empty()) {
        throw std::invalid_argument("OpenAI endpoint must not be empty");
    }
}

ProviderResult OpenAIResponsesProvider::rejected(std::string code,
                                                 std::string message)
{
    return ProviderResult{ProviderOutcome::rejected, {}, {},
                          std::move(code), std::move(message)};
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

    Json payload = {
        {"model", model_},
        {"input", request.input},
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
    Json document;
    try {
        document = Json::parse(response.body);
    } catch (const std::exception& error) {
        return rejected("openai_invalid_json_response", error.what());
    }

    if (response.status < 200 || response.status >= 300) {
        return rejected("openai_http_" + std::to_string(response.status),
                        provider_error_message(
                            document,
                            "OpenAI returned HTTP " + std::to_string(response.status)));
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
    if (*status == "incomplete") {
        return rejected("openai_incomplete", incomplete_reason(document));
    }
    if (*status == "failed") {
        return rejected("openai_failed",
                        provider_error_message(document, "OpenAI response failed"));
    }
    if (*status != "completed") {
        return rejected("openai_unexpected_status",
                        "unexpected OpenAI response status: " + *status);
    }

    const auto output = document.find("output");
    if (output == document.end() || !output->is_array()) {
        return rejected("openai_invalid_response",
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
                return rejected("openai_invalid_response",
                                "OpenAI output_text part has no text");
            }
            if (value->size() > request.max_output_bytes - text.size()) {
                return rejected("openai_output_too_large",
                                "OpenAI text output exceeds the task byte limit");
            }
            text += *value;
        }
    }

    if (refusal) {
        return rejected("openai_refusal", *refusal);
    }
    if (text.empty()) {
        return rejected("openai_no_text_output",
                        "completed OpenAI response contains no output_text");
    }

    return ProviderResult{ProviderOutcome::succeeded,
                          "text/plain; charset=utf-8",
                          std::move(text), {}, {}};
}

} // namespace gaudere_agent
