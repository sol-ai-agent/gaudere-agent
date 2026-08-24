#include "ResumeAfterWakeCognition.hpp"

#include <chrono>
#include <iostream>
#include <string>

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

class StubHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext&) override { return result; }
    HandlerResult result;
};

gaudere::work::Task fixture_task()
{
    gaudere::work::Task task;
    task.id = "resume-bounds";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain";
    task.input = "fixture";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 8192;
    task.limits.max_runtime = std::chrono::seconds{60};
    task.limits.max_attempts = 2;
    return task;
}

HandlerResult run(std::string output)
{
    StubHandler stub;
    stub.result = HandlerResult{HandlerOutcome::succeeded,
                                "text/plain",
                                std::move(output),
                                {}, {}, {}, {}};
    ResumeAfterWakeCognitionHandler cognition(stub);
    const auto task = fixture_task();
    return cognition.execute(TaskContext{task, [] { return false; }});
}

void test_reason_bounds()
{
    const auto exact = run(
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"stop\",\"reason\":\"" + std::string(1024, 'r') + "\"}");
    expect(exact.outcome == HandlerOutcome::succeeded,
           "1024-byte reason is accepted");

    const auto oversized = run(
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"stop\",\"reason\":\"" + std::string(1025, 'r') + "\"}");
    expect(oversized.outcome == HandlerOutcome::failed
               && oversized.failure_code == "cognition_invalid_resume_decision",
           "1025-byte reason fails closed");
}

void test_objective_bounds()
{
    const auto exact = run(
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"continue\",\"reason\":\"bounded\","
        "\"objective\":\"" + std::string(4096, 'o') + "\"}");
    expect(exact.outcome == HandlerOutcome::succeeded,
           "4096-byte objective is accepted");

    const auto oversized = run(
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"continue\",\"reason\":\"bounded\","
        "\"objective\":\"" + std::string(4097, 'o') + "\"}");
    expect(oversized.outcome == HandlerOutcome::failed
               && oversized.failure_code == "cognition_invalid_resume_decision",
           "4097-byte objective fails closed");
}

} // namespace

int main()
{
    test_reason_bounds();
    test_objective_bounds();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All resume cognition bounds tests passed\n";
    return 0;
}
