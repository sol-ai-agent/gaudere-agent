#include "OpenAIResponsesProvider.hpp"

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
        const HttpRequest&,
        std::optional<HttpSensitiveHeader>) override
    {
        ++calls;
        return result;
    }

    int calls = 0;
    HttpTransportResult result{
        HttpTransportOutcome::response,
        HttpResponse{200, {}, R"({"id":"resp","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"ok"}]}]})"},
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

} // namespace

int main()
{
    test_plain_http_endpoint_is_rejected();
    test_control_character_in_secret_is_rejected();
    test_non_json_http_error_preserves_status();
    test_provider_http_error_message_is_not_persisted();
    test_failed_status_error_message_is_not_persisted();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI Responses hardening tests passed\n";
    return 0;
}
