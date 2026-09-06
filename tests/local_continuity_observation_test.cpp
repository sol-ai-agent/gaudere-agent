#include "LocalContinuityObservation.hpp"
#include "LocalContinuityObservationHandler.hpp"

#include <gaudere/work/Task.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

gaudere_agent::LocalContinuityObservationFacts generation_one()
{
    gaudere_agent::LocalContinuityObservationFacts facts;
    facts.generation = 1;
    facts.due_at_ms = 1'000;
    facts.captured_at_ms = 1'250;
    facts.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    facts.provider_total = 10;
    facts.provider_limit = 12;
    facts.actions_total = 10;
    facts.actions_confirmed = 10;
    facts.wake_total = 1;
    facts.wake_fired = 1;
    facts.checkpoint_count = 1;
    facts.latest_checkpoint_task_id = facts.anchor_checkpoint_task_id;
    return facts;
}

} // namespace

int main()
{
    using namespace gaudere_agent;

    const auto facts = generation_one();
    const auto task = make_local_continuity_observation_task(facts);
    const auto inspection = inspect_local_continuity_observation_task(task);
    expect(inspection.eligible, "canonical generation-1 Task is accepted");
    expect(inspection.facts.generation == 1
               && inspection.facts.provider_total == 10
               && inspection.facts.actions_confirmed == 10
               && inspection.facts.wake_fired == 1,
           "canonical durable facts survive inspection");
    expect(task.kind == local_continuity_observation_task_kind
               && task.input_content_type == local_continuity_observation_content_type,
           "Task kind and content type are exact");

    auto later_capture = facts;
    later_capture.captured_at_ms += 60'000;
    later_capture.provider_total = 11;
    later_capture.actions_total = 11;
    const auto same_opportunity = make_local_continuity_observation_task(later_capture);
    expect(same_opportunity.id == task.id
               && same_opportunity.idempotency_key == task.idempotency_key,
           "capture time and observed facts do not change opportunity identity");
    expect(same_opportunity.input != task.input,
           "changed observed facts remain visible as a semantic conflict");

    auto changed_due = facts;
    changed_due.due_at_ms += 1;
    const auto different_opportunity = make_local_continuity_observation_task(changed_due);
    expect(different_opportunity.id != task.id,
           "durable due time participates in opportunity identity");

    auto tampered = task;
    tampered.input += " ";
    expect(!inspect_local_continuity_observation_task(tampered).eligible,
           "non-canonical payload bytes are rejected");

    auto wrong_identity = task;
    wrong_identity.id.back() = wrong_identity.id.back() == '0' ? '1' : '0';
    expect(!inspect_local_continuity_observation_task(wrong_identity).eligible,
           "Task identity tampering is rejected");

    bool invalid_anchor_rejected = false;
    try {
        auto bad = facts;
        bad.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:not-a-sha";
        (void)make_local_continuity_observation_task(bad);
    } catch (const std::invalid_argument&) {
        invalid_anchor_rejected = true;
    }
    expect(invalid_anchor_rejected,
           "non-canonical checkpoint anchor is rejected before Task creation");

    bool generation_one_predecessor_rejected = false;
    try {
        auto bad = facts;
        bad.predecessor_observation_task_id = task.id;
        bad.predecessor_observation_result_sha256 =
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        (void)make_local_continuity_observation_task(bad);
    } catch (const std::invalid_argument&) {
        generation_one_predecessor_rejected = true;
    }
    expect(generation_one_predecessor_rejected,
           "generation 1 cannot claim predecessor observation evidence");

    auto generation_two = facts;
    generation_two.generation = 2;
    generation_two.due_at_ms = 100'000;
    generation_two.captured_at_ms = 100'100;
    generation_two.predecessor_observation_task_id = task.id;
    generation_two.predecessor_observation_result_sha256 =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto second_task = make_local_continuity_observation_task(generation_two);
    expect(inspect_local_continuity_observation_task(second_task).eligible,
           "later generation requires and accepts exact predecessor evidence");

    bool generation_four_rejected = false;
    try {
        auto bad = generation_two;
        bad.generation = 4;
        (void)make_local_continuity_observation_task(bad);
    } catch (const std::invalid_argument&) {
        generation_four_rejected = true;
    }
    expect(generation_four_rejected,
           "generation cap prevents a fourth local observation");

    LocalContinuityObservationHandler handler;
    const TaskContext success_context{task, [] { return false; }};
    const auto handled = handler.execute(success_context);
    expect(handled.outcome == HandlerOutcome::succeeded
               && handled.content_type == local_continuity_observation_content_type
               && handled.output == task.input
               && handled.failure_code.empty()
               && handled.failure_message.empty(),
           "handler only echoes exact canonical observation bytes");

    const TaskContext cancelled_context{task, [] { return true; }};
    expect(handler.execute(cancelled_context).outcome == HandlerOutcome::cancelled,
           "handler cooperatively honors cancellation before work");

    const TaskContext invalid_context{tampered, [] { return false; }};
    const auto invalid_result = handler.execute(invalid_context);
    expect(invalid_result.outcome == HandlerOutcome::failed
               && invalid_result.failure_code == "invalid_local_continuity_observation",
           "handler fails closed on non-canonical Task input");

    auto completed = task;
    completed.status = gaudere::work::TaskStatus::succeeded;
    completed.result = gaudere::work::TaskResult{
        local_continuity_observation_content_type,
        completed.input,
        {}, {}};
    expect(canonical_local_continuity_observation_success(completed),
           "strict success inspection accepts exact echoed result");
    completed.result->output += "x";
    expect(!canonical_local_continuity_observation_success(completed),
           "strict success inspection rejects changed result bytes");

    if (failures != 0) {
        std::cerr << failures << " local continuity observation test(s) failed\n";
        return 1;
    }
    std::cout << "All local continuity observation tests passed\n";
    return 0;
}
