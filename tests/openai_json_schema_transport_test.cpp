#include "OpenAIJsonSchemaTransport.hpp"
#include "OpenAIResponsesProvider.hpp"
#include "ResumeDecisionStructuredOutput.hpp"

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

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_invalid(Function&& function, const std::string& message)
{
    try {
        function();
        expect(false, message);
    } catch (const std::invalid_argument&) {
    } catch (...) {
        expect(false, message + " (wrong exception type)");
    }
}

class FakeSecret final : public SecretSource {
public:
    std::optional<SecretValue> load(std::string_view) override
    {
        const std::string key = "synthetic-key";
        return SecretValue{std::vector<char>(key.begin(), key.end())};
    }
};

class CaptureTransport final : public HttpTransport {
public:
    HttpTransportResult perform(
        const HttpRequest& request,
        std::optional<HttpSensitiveHeader> sensitive_header) override
    {
        ++calls;
        last = request;
        saw_secret = sensitive_header.has_value();
        return HttpTransportResult{
            HttpTransportOutcome::response,
            HttpResponse{200, {},
                R"({"id":"r","status":"completed","output":[{"type":"message","content":[{"type":"output_text","text":"{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\",\"reason\":\"done\",\"objective\":null}"}]}]})"},
            {}, {}};
    }

    int calls = 0;
    bool saw_secret = false;
    HttpRequest last;
};

ProviderRequest request()
{
    ProviderRequest value;
    value.idempotency_key = "structured:test";
    value.content_type = "text/plain; charset=utf-8";
    value.input = "bounded cognition context";
    value.max_output_bytes = 8192;
    value.max_runtime = 5s;
    return value;
}

void test_generic_provider_remains_unstructured()
{
    CaptureTransport transport;
    FakeSecret secret;
    OpenAIResponsesProvider provider(
        transport, secret, "gpt-test", "key", "https://example.invalid/v1/responses");
    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::succeeded,
           "generic provider request succeeds through fake transport");
    const auto body = Json::parse(transport.last.body);
    expect(!body.contains("text"),
           "generic OpenAI provider payload does not acquire text.format");
}

void test_resume_contract_is_injected_exactly()
{
    CaptureTransport downstream;
    OpenAIJsonSchemaTransport structured(
        downstream, resume_decision_structured_output_contract());
    FakeSecret secret;
    OpenAIResponsesProvider provider(
        structured, secret, "gpt-test", "key", "https://example.invalid/v1/responses");

    const auto result = provider.invoke(request());
    expect(result.outcome == ProviderOutcome::succeeded,
           "structured request still uses existing provider response path");
    expect(downstream.calls == 1 && downstream.saw_secret,
           "decorator delegates exactly once and forwards borrowed secret");

    const auto body = Json::parse(downstream.last.body);
    expect(body.at("input") == "bounded cognition context",
           "decorator preserves model input bytes");
    expect(body.at("store") == false && body.at("stream") == false,
           "decorator preserves existing storage/streaming policy");
    const auto& format = body.at("text").at("format");
    expect(format.at("type") == "json_schema",
           "Responses text.format selects json_schema");
    expect(format.at("name") == "gaudere_resume_decision_v1",
           "structured format uses stable bounded cognition name");
    expect(format.at("strict") == true,
           "Structured Outputs strict mode is mandatory");

    const auto& schema = format.at("schema");
    expect(schema.at("type") == "object"
               && schema.at("additionalProperties") == false,
           "resume schema is a closed object");
    expect(schema.at("required")
               == Json::array({"schema", "decision", "reason", "objective"}),
           "all Structured Output fields are required");
    expect(schema.at("properties").at("decision").at("enum")
               == Json::array({"stop", "continue"}),
           "provider bounds decision vocabulary");
    expect(schema.at("properties").at("objective").at("type")
               == Json::array({"string", "null"}),
           "objective is required-but-nullable for strict schema compatibility");
}

void test_contract_validation_precedes_network()
{
    CaptureTransport downstream;
    expect_invalid(
        [&] {
            OpenAIJsonSchemaTransport transport(
                downstream, OpenAIJsonSchemaContract{"bad name!", R"({"type":"object"})"});
        },
        "invalid format name is rejected at construction");
    expect_invalid(
        [&] {
            OpenAIJsonSchemaTransport transport(
                downstream, OpenAIJsonSchemaContract{"valid_name", "not-json"});
        },
        "malformed schema is rejected at construction");
    expect_invalid(
        [&] {
            OpenAIJsonSchemaTransport transport(
                downstream, OpenAIJsonSchemaContract{"valid_name", "[]"});
        },
        "non-object schema is rejected at construction");
    expect(downstream.calls == 0,
           "invalid contracts cannot reach downstream transport");
}

} // namespace

int main()
{
    test_generic_provider_remains_unstructured();
    test_resume_contract_is_injected_exactly();
    test_contract_validation_precedes_network();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI JSON-schema transport tests passed\n";
    return 0;
}
