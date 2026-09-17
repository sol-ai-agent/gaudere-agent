#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <cassert>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(const char c) { return std::string(64, c); }

Task observation(const std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 10;
    facts.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('a');
    if (generation > 1) {
        assert(predecessor && predecessor->result);
        facts.predecessor_observation_task_id = predecessor->id;
        facts.predecessor_observation_result_sha256 =
            sha256_hex(predecessor->result->output);
    }
    facts.provider_scope = "provider.call:openai.responses";
    facts.provider_total = 10;
    facts.provider_limit = 12;
    facts.predecessor_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('b');
    facts.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('c');
    facts.historical_wake_scope = "cognition.reflect.wake.v0";
    facts.historical_wake_sha256 = hex('d');
    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{local_continuity_observation_content_type,
                             task.input, {}, {}};
    assert(canonical_local_continuity_observation_success(task));
    return task;
}

Task final_observation()
{
    const auto one = observation(1);
    const auto two = observation(2, one);
    return observation(3, two);
}

std::string decision(const std::string& kind,
                     const std::string& wake = "3600000")
{
    const auto wake_value = wake.empty() ? "null" : wake;
    return std::string{"{\"assessment\":\"cycle ok\",\"decision\":\""}
        + kind + "\",\"next_wake_after_ms\":" + wake_value
        + ",\"openai_request\":null,\"reason\":\"continue on my own schedule\","
          "\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}";
}

bool throws_make(const Task& anchor,
                 const std::string& model,
                 const std::uint64_t generation,
                 const std::int64_t due,
                 const std::int64_t captured,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    try {
        static_cast<void>(make_local_goose_cycle_task(
            anchor, model, generation, due, captured, predecessor));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

} // namespace

int main()
{
    const auto anchor = final_observation();
    const auto model = hex('e');

    const auto first = make_local_goose_cycle_task(
        anchor, model, 1, 10'000, 10'050);
    const auto first_again = make_local_goose_cycle_task(
        anchor, model, 1, 10'000, 10'050);
    assert(first.id == first_again.id);
    assert(first.idempotency_key == first_again.idempotency_key);
    assert(first.kind == local_goose_cycle_task_kind);
    assert(first.input_content_type == local_goose_cycle_content_type);

    const auto inspected = inspect_local_goose_cycle_task(first);
    assert(inspected.eligible);
    assert(inspected.anchor_observation_task_id == anchor.id);
    assert(inspected.anchor_observation_result_sha256
           == sha256_hex(anchor.result->output));
    assert(inspected.generation == 1);
    assert(inspected.due_at_ms == 10'000);
    assert(inspected.captured_at_ms == 10'050);
    assert(inspected.model_sha256 == model);
    assert(!inspected.predecessor);
    assert(inspected.canonical_input == first.input);

    const auto different_capture = make_local_goose_cycle_task(
        anchor, model, 1, 10'000, 10'051);
    assert(different_capture.id != first.id);

    auto succeeded_first = first;
    succeeded_first.status = TaskStatus::succeeded;
    succeeded_first.result = TaskResult{
        local_goose_decision_content_type, decision("continue_local"), {}, {}};
    assert(canonical_local_goose_cycle_success(succeeded_first));

    const auto second = make_local_goose_cycle_task(
        anchor, model, 2, 20'000, 20'001, succeeded_first);
    const auto second_inspection = inspect_local_goose_cycle_task(second);
    assert(second_inspection.eligible);
    assert(second_inspection.generation == 2);
    assert(second_inspection.predecessor);
    assert(second_inspection.predecessor->task_id == succeeded_first.id);
    assert(second_inspection.predecessor->result_sha256
           == sha256_hex(succeeded_first.result->output));
    assert(second_inspection.predecessor->decision.decision == "continue_local");
    assert(second_inspection.predecessor->decision.next_wake_after_ms == 3'600'000);

    auto tampered = second;
    tampered.input += " ";
    assert(!inspect_local_goose_cycle_task(tampered).eligible);

    tampered = second;
    tampered.limits.max_attempts = 3;
    assert(!inspect_local_goose_cycle_task(tampered).eligible);

    assert(throws_make(anchor, model, 0, 1, 1));
    assert(throws_make(anchor, model, 1, 10, 9));
    assert(throws_make(anchor, model, 1, 10, 10, succeeded_first));
    assert(throws_make(anchor, model, 2, 10, 10));

    const auto early_anchor = observation(1);
    assert(throws_make(early_anchor, model, 1, 10, 10));

    auto failed_predecessor = succeeded_first;
    failed_predecessor.status = TaskStatus::failed;
    assert(throws_make(anchor, model, 2, 20, 20, failed_predecessor));

    auto changed_model_predecessor = make_local_goose_cycle_task(
        anchor, hex('f'), 1, 10'000, 10'050);
    changed_model_predecessor.status = TaskStatus::succeeded;
    changed_model_predecessor.result = TaskResult{
        local_goose_decision_content_type, decision("continue_local"), {}, {}};
    assert(canonical_local_goose_cycle_success(changed_model_predecessor));
    assert(throws_make(anchor, model, 2, 20, 20, changed_model_predecessor));

    const auto prompt = local_goose_cycle_prompt(second_inspection);
    assert(prompt.find("Reason as Gaudere, for Gaudere") != std::string::npos);
    assert(prompt.find("request_openai") != std::string::npos);
    assert(prompt.find("next_wake_after_ms is your scheduling intent")
           != std::string::npos);
    assert(prompt.find(second.input) != std::string::npos);

    auto bad_result = succeeded_first;
    bad_result.result->output = "not-json";
    assert(!canonical_local_goose_cycle_success(bad_result));

    std::cout << "local Goose cycle contract: ok\n";
    return 0;
}
