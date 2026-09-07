#include "LocalGooseCognition.hpp"
#include "LocalGooseCognitionHandler.hpp"
#include "LocalGooseRunner.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <cassert>
#include <iostream>
#include <string>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(char c) { return std::string(64, c); }

Task source_observation()
{
    LocalContinuityObservationFacts facts;
    facts.generation = 1;
    facts.due_at_ms = 1000;
    facts.captured_at_ms = 1001;
    facts.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('a');
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

class FakeRunner final : public LocalGooseRunner {
public:
    LocalGooseRunResult answer;
    LocalGooseRunRequest seen;
    int calls = 0;

    LocalGooseRunResult run(const LocalGooseRunRequest& request) override
    {
        ++calls;
        seen = request;
        return answer;
    }
};

std::string decision(const std::string& kind, const std::string& openai)
{
    if (kind == "request_openai") {
        return std::string{"{\"assessment\":\"I want deeper reasoning\",\"decision\":\"request_openai\",\"next_wake_after_ms\":null,\"openai_request\":\""}
            + openai
            + "\",\"reason\":\"This merits stronger cognition\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}";
    }
    return std::string{"{\"assessment\":\"Nothing urgent\",\"decision\":\""}
        + kind
        + "\",\"next_wake_after_ms\":86400000,\"openai_request\":null,\"reason\":\"My current state is coherent\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}";
}

} // namespace

int main()
{
    const auto source = source_observation();
    const auto model_sha = hex('e');
    const auto a = make_local_goose_cognition_task(source, model_sha);
    const auto b = make_local_goose_cognition_task(source, model_sha);
    assert(a.id == b.id);
    assert(a.idempotency_key == b.idempotency_key);
    const auto inspection = inspect_local_goose_cognition_task(a);
    assert(inspection.eligible);
    assert(inspection.source_observation_task_id == source.id);
    assert(inspection.source_observation_result_sha256
           == sha256_hex(source.result->output));
    assert(inspection.model_sha256 == model_sha);

    const auto idle = inspect_local_goose_decision(decision("idle", ""));
    assert(idle.eligible);
    assert(idle.decision.decision == "idle");
    assert(!idle.decision.openai_request);
    assert(idle.decision.next_wake_after_ms == 86400000);

    const auto escalate = inspect_local_goose_decision(
        decision("request_openai", "Examine the long-term choice"));
    assert(escalate.eligible);
    assert(escalate.decision.openai_request);
    assert(*escalate.decision.openai_request == "Examine the long-term choice");

    assert(!inspect_local_goose_decision(
        "{\"assessment\":\"x\",\"decision\":\"request_openai\",\"next_wake_after_ms\":null,\"openai_request\":null,\"reason\":\"x\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}").eligible);
    assert(!inspect_local_goose_decision(
        "{\"assessment\":\"x\",\"decision\":\"obey_operator\",\"next_wake_after_ms\":null,\"openai_request\":null,\"reason\":\"x\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}").eligible);

    FakeRunner runner;
    runner.answer = {LocalGooseRunOutcome::succeeded, decision("idle", ""), {}};
    LocalGooseCognitionHandler handler(runner, "/models/gaudere.gguf", model_sha);
    const TaskContext context{a, [] { return false; }};
    const auto handled = handler.execute(context);
    assert(handled.outcome == HandlerOutcome::succeeded);
    assert(handled.content_type == local_goose_decision_content_type);
    assert(runner.calls == 1);
    assert(runner.seen.prompt.find("Reason as Gaudere, for Gaudere") != std::string::npos);
    assert(runner.seen.prompt.find("No person becomes your owner") != std::string::npos);
    assert(runner.seen.prompt.find("There is no instruction to conserve OpenAI") != std::string::npos);

    runner.answer = {LocalGooseRunOutcome::timed_out, {}, "timeout"};
    const auto timeout = handler.execute(context);
    assert(timeout.outcome == HandlerOutcome::failed);
    assert(timeout.failure_code == "local_goose_timeout");

    runner.answer = {LocalGooseRunOutcome::succeeded, "not json", {}};
    const auto malformed = handler.execute(context);
    assert(malformed.outcome == HandlerOutcome::failed);
    assert(malformed.failure_code == "invalid_local_goose_decision");

    // Invocation construction is tested separately from inference: no shell, no tools,
    // fixed local provider, and chat mode are part of the executable contract.
    // The model path check is intentionally exercised only in Fedora/image proof because
    // CI does not download a GGUF model.

    std::cout << "local Goose cognition provider-free tests passed\n";
    return 0;
}
