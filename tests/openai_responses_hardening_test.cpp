#include "OpenAIResponsesProvider.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;
using Json = nlohmann::json;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class Secret final : public SecretSource {
public:
    std::optional<SecretValue> load(std::string_view) override
    {
        return SecretValue{std::vector<char>(value.begin(), value.end())};
    }

    std::string value = "synthetic-api-key";
};

class Transport final : public HttpTransport {
public:
    HttpTransportResult perform(
        const HttpRequest& request,
        std::optional<HttpSensitiveHeader>) override
    {
        ++calls;
        last_request = request;
        return result;
    }

    int calls = 0;
    HttpRequest last_request;
    HttpTransportResult result{
        HttpTransportOutcome::response,
        HttpResponse{200, {}, R"({"id":"resp","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}],"usage":{"input_tokens":3,"input_tokens_details":{"cached_tokens":1,"cache_write_tokens":0},"output_tokens":2,"output_tokens_details":{"reasoning_tokens":1},"total_tokens":5}})"},
        {}, {}};
};

ProviderRequest request()
{
    ProviderRequest value;
    value.idempotency_key = "hardening";
    value.content_type = "text/plain";
    value.input = "hello";
    value.max_output_bytes = 1024;
    value.max_runtime = 1s;
    return value;
}

void test_plain_http_endpoint_is_rejected()
{
    Secret secret;
    Transport transport;
    try {
        OpenAIResponsesProvider provider(
            transport, secret, "gpt-test", "openai-api-key",
            "http://api.openai.invalid/v1/responses");
        static_cast<void>(provider);
        expect(false, "plain HTTP OpenAI endpoint is rejected");
    } catch (const std::invalid_argument&) {
        // Expected.
    } catch (...) {
        expect(false, "plain HTTP endpoint throws invalid_argument");
    }
}

void test_control_character_in_secret_is_rejected()
{
    Secret secret;
    Transport transport;
    secret.value = "synthetic-api-key\n";
    OpenAIResponsesProvider provider(transport, secret, "gpt-test");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::rejected
               && result.failure_code == "openai_secret_invalid",
           "secret containing newline is rejected");
    expect(transport.calls == 0,
           "invalid bearer secret never reaches HTTP transport");
}

void test_provider_sets_explicit_output_token_cap()
{
    Secret secret;
    Transport transport;
    OpenAIResponsesProvider provider(transport, secret, "gpt-test");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::succeeded,
           "bounded output-token fixture succeeds");
    const auto body = Json::parse(transport.last_request.body);
    expect(body.contains("max_output_tokens")
               && body.at("max_output_tokens").is_number_unsigned()
               && body.at("max_output_tokens").get<std::uint64_t>() == 1024,
           "every OpenAI Responses request has an explicit 1024-token generation cap");

    expect(result.metadata_content_type
               == "application/vnd.gaudere.provider-usage+json",
           "successful production response exposes normalized usage metadata");
    const auto usage = Json::parse(result.metadata);
    expect(usage.at("schema") == "gaudere.provider_usage.v1"
               && usage.at("provider") == "openai"
               && usage.at("model") == "gpt-test"
               && usage.at("input_tokens") == 3
               && usage.at("cached_input_tokens") == 1
               && usage.at("cache_write_input_tokens") == 0
               && usage.at("output_tokens") == 2
               && usage.at("reasoning_tokens") == 1
               && usage.at("total_tokens") == 5,
           "OpenAI usage is reduced to trusted token counters only");
}

void test_non_json_http_error_preserves_status()
{
    Secret secret;
    Transport transport;
    transport.result.response = HttpResponse{429, {}, "plain rate-limit response"};
    OpenAIResponsesProvider provider(transport, secret, "gpt-test");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::rejected
               && result.failure_code == "openai_http_429"
               && result.failure_message == "OpenAI returned HTTP 429",
           "definite non-JSON HTTP error keeps its status classification");
}

void test_provider_http_error_message_is_not_persisted()
{
    Secret secret;
    Transport transport;
    transport.result.response = HttpResponse{
        401, {},
        R"({"error":{"message":"Incorrect API key provided: synthetic-api-key-secret-fragment","type":"invalid_request_error"}})"};
    OpenAIResponsesProvider provider(transport, secret, "gpt-test");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::rejected
               && result.failure_code == "openai_http_401"
               && result.failure_message == "OpenAI returned HTTP 401",
           "provider HTTP error body is replaced by trusted status-only diagnostic");
    expect(result.failure_message.find("synthetic-api-key") == std::string::npos,
           "credential-like provider text is absent from durable failure message");
    expect(result.metadata.empty(),
           "raw HTTP error body cannot become durable usage metadata");
}

void test_failed_status_error_message_is_not_persisted()
{
    Secret secret;
    Transport transport;
    transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp","status":"failed","error":{"message":"sensitive-provider-error-text"},"output":[]})"};
    OpenAIResponsesProvider provider(transport, secret, "gpt-test");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::rejected
               && result.failure_code == "openai_failed"
               && result.failure_message == "OpenAI response reported failed status",
           "failed response uses a trusted normalized durable diagnostic");
    expect(result.failure_message.find("sensitive-provider-error-text") == std::string::npos,
           "provider failed-status error text is not durably retained");
}

void test_missing_or_malformed_usage_fails_closed_for_production_success()
{
    Secret secret;

    Transport missing_transport;
    missing_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}]})"};
    OpenAIResponsesProvider missing_provider(missing_transport, secret, "gpt-test");
    const auto missing = missing_provider.invoke(request());
    expect(missing.outcome == ProviderOutcome::rejected
               && missing.failure_code == "openai_usage_missing",
           "production success without usage cannot silently lose accounting");

    Transport malformed_transport;
    malformed_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}],"usage":{"input_tokens":-1,"output_tokens":2,"total_tokens":1}})"};
    OpenAIResponsesProvider malformed_provider(malformed_transport, secret, "gpt-test");
    const auto malformed = malformed_provider.invoke(request());
    expect(malformed.outcome == ProviderOutcome::rejected
               && malformed.failure_code == "openai_invalid_usage",
           "malformed production usage is rejected instead of persisted");
}

} // namespace

int main()
{
    test_plain_http_endpoint_is_rejected();
    test_control_character_in_secret_is_rejected();
    test_provider_sets_explicit_output_token_cap();
    test_non_json_http_error_preserves_status();
    test_provider_http_error_message_is_not_persisted();
    test_failed_status_error_message_is_not_persisted();
    test_missing_or_malformed_usage_fails_closed_for_production_success();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI Responses hardening tests passed\n";
    return 0;
}
