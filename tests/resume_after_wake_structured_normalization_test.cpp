#include "ResumeAfterWakeCognition.hpp"

#include <chrono>
#include <iostream>
#include <string>

namespace {

using namespace gaudere_agent;

int failures = 0;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class StubHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext&) override { return result; }
    HandlerResult result;
};

HandlerResult run(const std::string& output)
{
    StubHandler stub;
    stub.result = HandlerResult{HandlerOutcome::succeeded,
                                "text/plain; charset=utf-8",
                                output, {}, {}, {}, {}};
    ResumeAfterWakeCognitionHandler cognition(stub);
    gaudere::work::Task task;
    task.id = "structured-normalization";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain";
    task.input = "fixture";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 8192;
    task.limits.max_runtime = std::chrono::seconds{60};
    task.limits.max_attempts = 2;
    return cognition.execute(TaskContext{task, [] { return false; }});
}

void test_stop_null_normalizes_to_historical_shape()
{
    const auto result = run(
        R"({"schema":"gaudere.cognition.resume-decision.v1","decision":"stop","reason":"No useful work remains.","objective":null})");
    expect(result.outcome == HandlerOutcome::succeeded,
           "stop with Structured Output null objective succeeds");
    expect(result.content_type == resume_after_wake_decision_content_type,
           "normalized result keeps durable cognition decision content type");
    expect(result.output
               == R"({"decision":"stop","reason":"No useful work remains.","schema":"gaudere.cognition.resume-decision.v1"})",
           "stop null objective is removed from durable historical shape");
}

void test_semantic_correlation_still_fails_closed()
{
    const auto stop_with_text = run(
        R"({"schema":"gaudere.cognition.resume-decision.v1","decision":"stop","reason":"done","objective":"should not exist"})");
    expect(stop_with_text.outcome == HandlerOutcome::failed
               && stop_with_text.failure_code == "cognition_invalid_resume_decision",
           "stop with textual objective fails closed");

    const auto continue_with_null = run(
        R"({"schema":"gaudere.cognition.resume-decision.v1","decision":"continue","reason":"more work","objective":null})");
    expect(continue_with_null.outcome == HandlerOutcome::failed
               && continue_with_null.failure_code == "cognition_invalid_resume_decision",
           "continue with null objective fails closed");
}

void test_continue_string_remains_canonical()
{
    const auto result = run(
        R"({"schema":"gaudere.cognition.resume-decision.v1","decision":"continue","reason":"A concrete next step remains.","objective":"Record one fresh bounded cognition cycle."})");
    expect(result.outcome == HandlerOutcome::succeeded,
           "continue with textual objective succeeds");
    expect(result.output
               == R"({"decision":"continue","objective":"Record one fresh bounded cognition cycle.","reason":"A concrete next step remains.","schema":"gaudere.cognition.resume-decision.v1"})",
           "continue Structured Output normalizes canonically");
}

} // namespace

int main()
{
    test_stop_null_normalizes_to_historical_shape();
    test_semantic_correlation_still_fails_closed();
    test_continue_string_remains_canonical();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All Structured Output cognition normalization tests passed\n";
    return 0;
}
