#include "OpenAIResponsesProvider.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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

template <typename Exception, typename Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        std::forward<Function>(function)();
        expect(false, message);
    } catch (const Exception&) {
        // Expected.
    } catch (...) {
        expect(false, message + " (wrong exception type)");
    }
}

std::vector<char> bytes(const std::string_view value)
{
    return std::vector<char>(value.begin(), value.end());
}

class FakeSecretSource final : public SecretSource {
public:
    std::optional<SecretValue> load(const std::string_view name) override
    {
        ++loads;
        last_name = std::string(name);
        if (throw_on_load) {
            throw std::runtime_error("synthetic secret failure");
        }
        if (!present) {
            return std::nullopt;
        }
        return SecretValue{bytes(value)};
    }

    bool present = true;
    bool throw_on_load = false;
    std::string value = "synthetic-api-key";
    int loads = 0;
    std::string last_name;
};

class FakeHttpTransport final : public HttpTransport {
public:
    HttpTransportResult perform(
        const HttpRequest& request,
        const std::optional<HttpSensitiveHeader> sensitive_header) override
    {
        ++calls;
        last_request = request;
        saw_sensitive_header = sensitive_header.has_value();
        if (sensitive_header) {
            sensitive_name_ok = sensitive_header->name == "Authorization";
            sensitive_prefix_ok = sensitive_header->prefix == "Bearer ";
            sensitive_value_ok = sensitive_header->value == expected_secret;
        }
        if (throw_on_perform) {
            throw std::runtime_error("synthetic transport exception");
        }
        return result;
    }

    HttpTransportResult result{
        HttpTransportOutcome::response,
        HttpResponse{200, {}, R"({"id":"resp_default","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"default"}]}]})"},
        {}, {}};
    bool throw_on_perform = false;
    std::string expected_secret = "synthetic-api-key";
    int calls = 0;
    bool saw_sensitive_header = false;
    bool sensitive_name_ok = false;
    bool sensitive_prefix_ok = false;
    bool sensitive_value_ok = false;
    HttpRequest last_request;
};

ProviderRequest request(std::string input = "Bonjour Gaudere")
{
    ProviderRequest value;
    value.idempotency_key = "provider.call:openai.responses:task-key:first";
    value.content_type = "text/plain; charset=utf-8";
    value.input = std::move(input);
    value.max_output_bytes = 4096;
    value.max_runtime = 3s;
    return value;
}

std::optional<std::string> header(const HttpRequest& request,
                                  const std::string_view name)
{
    for (const auto& item : request.headers) {
        if (item.name == name) {
            return item.value;
        }
    }
    return std::nullopt;
}

void test_constructor_validation()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    expect_throw<std::invalid_argument>(
        [&] { OpenAIResponsesProvider provider(transport, secrets, ""); },
        "empty model is rejected");
    expect_throw<std::invalid_argument>(
        [&] { OpenAIResponsesProvider provider(transport, secrets, "gpt-test", ""); },
        "empty secret name is rejected");
    expect_throw<std::invalid_argument>(
        [&] {
            OpenAIResponsesProvider provider(
                transport, secrets, "gpt-test", "openai-api-key", "");
        },
        "empty endpoint is rejected");
}

void test_successful_request_shape_and_text_aggregation()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    transport.result.response = HttpResponse{
        200,
        {{"x-request-id", "req_test"}},
        R"({"id":"resp_1","status":"completed","output":[{"type":"reasoning","summary":[]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Bonjour "},{"type":"output_text","text":"Gaudere"}]}]})"};

    OpenAIResponsesProvider provider(
        transport, secrets, "gpt-test", "openai-key", "https://example.invalid/v1/responses");
    const auto result = provider.invoke(request());

    expect(result.outcome == ProviderOutcome::succeeded,
           "completed text response succeeds");
    expect(result.content_type == "text/plain; charset=utf-8",
           "successful result is UTF-8 text");
    expect(result.output == "Bonjour Gaudere",
           "all output_text parts are aggregated in order");
    expect(transport.calls == 1, "transport is called exactly once");
    expect(secrets.loads == 1 && secrets.last_name == "openai-key",
           "configured secret is loaded once");

    expect(transport.last_request.method == "POST",
           "Responses request uses POST");
    expect(transport.last_request.url == "https://example.invalid/v1/responses",
           "configured endpoint is used");
    expect(transport.last_request.timeout == 3s,
           "task runtime bound reaches HTTP transport");
    expect(transport.last_request.max_response_bytes >= 64 * 1024
               && transport.last_request.max_response_bytes <= 1024 * 1024,
           "raw HTTP response has an explicit bounded envelope");

    const auto body = Json::parse(transport.last_request.body);
    expect(body.at("model") == "gpt-test", "request contains configured model");
    expect(body.at("input") == "Bonjour Gaudere", "request contains task text");
    expect(body.at("store") == false, "Responses storage is explicitly disabled");
    expect(body.at("stream") == false, "first adapter is explicitly non-streaming");

    expect(header(transport.last_request, "Content-Type") == "application/json",
           "request declares JSON content");
    expect(header(transport.last_request, "Accept") == "application/json",
           "request asks for JSON response");
    const auto client_id = header(transport.last_request, "X-Client-Request-Id");
    expect(client_id.has_value() && client_id->rfind("gaudere-", 0) == 0
               && client_id->size() == 40,
           "request carries a compact deterministic client request ID");
    expect(*client_id == OpenAIResponsesProvider::client_request_id(
                             "provider.call:openai.responses:task-key:first"),
           "client request ID is derived deterministically from durable identity");
    expect(header(transport.last_request, "Authorization") == std::nullopt,
           "API key is absent from ordinary copyable request headers");
    expect(transport.saw_sensitive_header && transport.sensitive_name_ok
               && transport.sensitive_prefix_ok && transport.sensitive_value_ok,
           "authorization is supplied only through the borrowed sensitive header");
}

void test_transport_unknown()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    transport.result = HttpTransportResult{
        HttpTransportOutcome::effect_unknown, std::nullopt,
        "timeout_after_send", "synthetic timeout"};

    OpenAIResponsesProvider provider(transport, secrets, "gpt-test");
    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::effect_unknown
               && result.failure_code == "timeout_after_send",
           "ambiguous HTTP transport maps to unknown provider effect");
}

void test_transport_exception()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    transport.throw_on_perform = true;

    OpenAIResponsesProvider provider(transport, secrets, "gpt-test");
    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::effect_unknown
               && result.failure_code == "openai_transport_exception",
           "transport exception is never treated as safe retry evidence");
}

void test_http_error_is_definite_rejection()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    transport.result.response = HttpResponse{
        401, {},
        R"({"error":{"message":"invalid synthetic credential","type":"invalid_request_error"}})"};

    OpenAIResponsesProvider provider(transport, secrets, "gpt-test");
    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::rejected
               && result.failure_code == "openai_http_401"
               && result.failure_message == "invalid synthetic credential",
           "HTTP error response is a definite provider rejection");
}

void test_incomplete_and_failed_responses()
{
    FakeSecretSource secrets;

    FakeHttpTransport incomplete_transport;
    incomplete_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp_inc","status":"incomplete","incomplete_details":{"reason":"max_output_tokens"},"output":[]})"};
    OpenAIResponsesProvider incomplete_provider(
        incomplete_transport, secrets, "gpt-test");
    const auto incomplete = incomplete_provider.invoke(request());
    expect(incomplete.outcome == ProviderOutcome::rejected
               && incomplete.failure_code == "openai_incomplete"
               && incomplete.failure_message == "max_output_tokens",
           "incomplete response is definite but not successful work");

    FakeHttpTransport failed_transport;
    failed_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp_fail","status":"failed","error":{"message":"model failed"},"output":[]})"};
    OpenAIResponsesProvider failed_provider(failed_transport, secrets, "gpt-test");
    const auto failed = failed_provider.invoke(request());
    expect(failed.outcome == ProviderOutcome::rejected
               && failed.failure_code == "openai_failed"
               && failed.failure_message == "model failed",
           "failed response is a definite provider rejection");
}

void test_refusal_and_no_text()
{
    FakeSecretSource secrets;

    FakeHttpTransport refusal_transport;
    refusal_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp_refusal","status":"completed","output":[{"type":"message","content":[{"type":"refusal","refusal":"synthetic refusal"}]}]})"};
    OpenAIResponsesProvider refusal_provider(refusal_transport, secrets, "gpt-test");
    const auto refusal = refusal_provider.invoke(request());
    expect(refusal.outcome == ProviderOutcome::rejected
               && refusal.failure_code == "openai_refusal"
               && refusal.failure_message == "synthetic refusal",
           "model refusal is represented as definite semantic failure");

    FakeHttpTransport empty_transport;
    empty_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp_empty","status":"completed","output":[{"type":"reasoning","summary":[]}]})"};
    OpenAIResponsesProvider empty_provider(empty_transport, secrets, "gpt-test");
    const auto empty = empty_provider.invoke(request());
    expect(empty.outcome == ProviderOutcome::rejected
               && empty.failure_code == "openai_no_text_output",
           "completed response without text is rejected explicitly");
}

void test_invalid_json_and_output_limit()
{
    FakeSecretSource secrets;

    FakeHttpTransport invalid_transport;
    invalid_transport.result.response = HttpResponse{200, {}, "not-json"};
    OpenAIResponsesProvider invalid_provider(invalid_transport, secrets, "gpt-test");
    const auto invalid = invalid_provider.invoke(request());
    expect(invalid.outcome == ProviderOutcome::rejected
               && invalid.failure_code == "openai_invalid_json_response",
           "malformed definite HTTP response is not classified as transport ambiguity");

    FakeHttpTransport large_transport;
    large_transport.result.response = HttpResponse{
        200, {},
        R"({"id":"resp_large","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"12345"}]}]})"};
    OpenAIResponsesProvider large_provider(large_transport, secrets, "gpt-test");
    auto bounded = request();
    bounded.max_output_bytes = 4;
    const auto large = large_provider.invoke(bounded);
    expect(large.outcome == ProviderOutcome::rejected
               && large.failure_code == "openai_output_too_large",
           "parsed text cannot exceed durable task output byte bound");
}

void test_pre_transport_validation_and_secret_failures()
{
    FakeHttpTransport transport;
    FakeSecretSource secrets;
    OpenAIResponsesProvider provider(transport, secrets, "gpt-test");

    auto unsupported = request();
    unsupported.content_type = "application/json";
    const auto unsupported_result = provider.invoke(unsupported);
    expect(unsupported_result.outcome == ProviderOutcome::rejected
               && unsupported_result.failure_code == "openai_unsupported_input_type"
               && transport.calls == 0 && secrets.loads == 0,
           "unsupported input is rejected before secret or transport access");

    auto excessive = request();
    excessive.max_output_bytes = 256 * 1024 + 1;
    const auto excessive_result = provider.invoke(excessive);
    expect(excessive_result.outcome == ProviderOutcome::rejected
               && excessive_result.failure_code == "openai_unsupported_output_limit"
               && transport.calls == 0 && secrets.loads == 0,
           "unsupported output bound is rejected before secret access");

    secrets.present = false;
    const auto missing = provider.invoke(request());
    expect(missing.outcome == ProviderOutcome::rejected
               && missing.failure_code == "openai_secret_missing"
               && transport.calls == 0,
           "missing credential never reaches transport");

    secrets.present = true;
    secrets.throw_on_load = true;
    const auto secret_error = provider.invoke(request());
    expect(secret_error.outcome == ProviderOutcome::rejected
               && secret_error.failure_code == "openai_secret_error"
               && transport.calls == 0,
           "secret-source failure never reaches transport");
}

void test_client_request_id_is_stable_and_distinguishes_keys()
{
    const auto first = OpenAIResponsesProvider::client_request_id("one");
    const auto again = OpenAIResponsesProvider::client_request_id("one");
    const auto second = OpenAIResponsesProvider::client_request_id("two");
    expect(first == again, "client request ID is stable within the implementation");
    expect(first != second, "different durable identities produce different client IDs");
    expect(first.size() <= 512, "client request ID fits OpenAI documented limit");
}

} // namespace

int main()
{
    test_constructor_validation();
    test_successful_request_shape_and_text_aggregation();
    test_transport_unknown();
    test_transport_exception();
    test_http_error_is_definite_rejection();
    test_incomplete_and_failed_responses();
    test_refusal_and_no_text();
    test_invalid_json_and_output_limit();
    test_pre_transport_validation_and_secret_failures();
    test_client_request_id_is_stable_and_distinguishes_keys();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI Responses provider tests passed\n";
    return 0;
}
