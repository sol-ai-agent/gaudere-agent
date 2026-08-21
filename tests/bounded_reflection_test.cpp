#include "BoundedReflection.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace gaudere_agent;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeHandler final : public TaskHandler {
public:
    explicit FakeHandler(HandlerResult value) : value_(std::move(value)) {}

    HandlerResult execute(const TaskContext& context) override
    {
        ++calls;
        last_kind = context.task.kind;
        return value_;
    }

    int calls = 0;
    std::string last_kind;

private:
    HandlerResult value_;
};

HandlerResult provider_success(std::string output)
{
    return HandlerResult{
        HandlerOutcome::succeeded,
        "text/plain; charset=utf-8",
        std::move(output), {}, {},
        "application/vnd.gaudere.provider-usage+json",
        "{\"total_tokens\":7}"};
}

HandlerResult execute(std::string output)
{
    auto task = make_bounded_reflection_task("reflection-test", "Choose safely.");
    FakeHandler provider(provider_success(std::move(output)));
    BoundedReflectionHandler reflection(provider);
    const TaskContext context{task, [] { return false; }};
    auto result = reflection.execute(context);
    expect(provider.calls == 1,
           "bounded reflection delegates exactly once to provider handler");
    expect(provider.last_kind == bounded_reflection_task_kind,
           "provider handler receives bounded reflection task kind");
    return result;
}

void test_task_factory()
{
    const auto task = make_bounded_reflection_task(
        "reflect-001", "Decide whether another bounded wake is useful.");
    expect(task.id == "reflect-001"
               && task.idempotency_key == "cognition.reflect.v1:reflect-001"
               && task.kind == bounded_reflection_task_kind,
           "factory assigns deterministic bounded reflection identity");
    expect(task.input_content_type == "text/plain; charset=utf-8"
               && task.input.find("another bounded wake") != std::string::npos
               && task.input.find("proposal only") != std::string::npos,
           "factory wraps objective in fixed proposal-only prompt");
    expect(task.limits.max_input_bytes == 16 * 1024
               && task.limits.max_output_bytes == 4096
               && task.limits.max_runtime == std::chrono::seconds{60}
               && task.limits.max_attempts == 2,
           "factory applies hard reflection resource bounds");

    const auto maximum = make_bounded_reflection_task(
        "reflect-max", std::string(4096, '"'));
    expect(maximum.input.size() <= maximum.limits.max_input_bytes,
           "maximum quoted objective remains inside task input limit");

    for (const auto& invalid : std::vector<std::string>{
             {}, std::string(4097, 'x'), std::string{"bad\0control", 11},
             std::string{"\xc3\x28", 2}}) {
        try {
            static_cast<void>(make_bounded_reflection_task("invalid", invalid));
            expect(false, "invalid objective is rejected by task factory");
        } catch (const std::invalid_argument&) {
            expect(true, "invalid objective rejection is explicit");
        }
    }
}

void test_valid_stop_decision()
{
    const auto result = execute(
        "{\"reason\":\"Nothing else is needed.\","
        "\"decision\":\"stop\","
        "\"schema\":\"gaudere.cognition.decision.v1\"}");
    expect(result.outcome == HandlerOutcome::succeeded
               && result.content_type == bounded_reflection_decision_content_type,
           "valid stop decision succeeds with normalized content type");
    expect(nlohmann::json::parse(result.output)
               == nlohmann::json{{"schema", "gaudere.cognition.decision.v1"},
                                 {"decision", "stop"},
                                 {"reason", "Nothing else is needed."}},
           "valid stop decision is canonicalized without new authority");
    expect(result.metadata_content_type
               == "application/vnd.gaudere.provider-usage+json"
               && result.metadata == "{\"total_tokens\":7}",
           "normalization preserves separate provider usage metadata");
}

void test_valid_wake_proposals()
{
    for (const auto seconds : {900, 86400}) {
        const auto result = execute(
            "{\"schema\":\"gaudere.cognition.decision.v1\","
            "\"decision\":\"propose_wake\","
            "\"reason\":\"Revisit within the bounded horizon.\","
            "\"wake_after_seconds\":" + std::to_string(seconds) + "}");
        const auto decision = nlohmann::json::parse(result.output);
        expect(result.outcome == HandlerOutcome::succeeded
                   && decision.at("decision") == "propose_wake"
                   && decision.at("wake_after_seconds") == seconds,
               "wake proposal accepts inclusive hard delay boundary");
    }
}

void test_invalid_decisions_fail_definitely()
{
    const std::string long_reason(1025, 'r');
    const std::vector<std::string> invalid = {
        "not json",
        "[]",
        "{}",
        "{\"schema\":\"wrong\",\"decision\":\"stop\",\"reason\":\"x\"}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"run\",\"reason\":\"x\"}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"stop\",\"reason\":\"\"}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"stop\",\"reason\":\""
            + long_reason + "\"}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"stop\",\"reason\":\"x\",\"extra\":true}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"stop\",\"reason\":\"x\",\"wake_after_seconds\":900}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\"}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\",\"wake_after_seconds\":899}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\",\"wake_after_seconds\":86401}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\",\"wake_after_seconds\":900.0}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\",\"wake_after_seconds\":true}",
        "{\"schema\":\"gaudere.cognition.decision.v1\",\"decision\":\"propose_wake\",\"reason\":\"x\",\"wake_after_seconds\":\"900\"}"
    };

    for (const auto& value : invalid) {
        const auto result = execute(value);
        expect(result.outcome == HandlerOutcome::failed
                   && result.failure_code == "cognition_invalid_decision"
                   && result.output.empty() && result.content_type.empty(),
               "invalid model decision becomes definite bounded failure");
        expect(result.metadata == "{\"total_tokens\":7}",
               "invalid decision still preserves definite provider usage");
    }
}

void test_non_success_provider_result_passes_through()
{
    auto task = make_bounded_reflection_task("provider-review", "Reflect once.");
    HandlerResult provider_result{HandlerOutcome::manual_review, {}, {},
                                  "provider_effect_unknown", "review required"};
    FakeHandler provider(provider_result);
    BoundedReflectionHandler reflection(provider);
    const auto result = reflection.execute(TaskContext{task, [] { return false; }});
    expect(result.outcome == HandlerOutcome::manual_review
               && result.failure_code == "provider_effect_unknown"
               && provider.calls == 1,
           "ambiguous provider result passes through without reinterpretation");
}

} // namespace

int main()
{
    test_task_factory();
    test_valid_stop_decision();
    test_valid_wake_proposals();
    test_invalid_decisions_fail_definitely();
    test_non_success_provider_result_passes_through();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All bounded reflection tests passed\n";
    return 0;
}
