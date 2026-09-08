#include "LocalGooseCognition.hpp"
#include "LocalGooseCognitionHandler.hpp"
#include "LocalGooseCognitionService.hpp"
#include "LocalGooseRunner.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using TimePoint = gaudere::work::TimePoint;

std::string hex(char c) { return std::string(64, c); }

Task source_observation(const std::uint32_t generation = 1,
                        const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 1;
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

class MemoryTaskStore final : public gaudere::work::TaskStore {
public:
    std::optional<Task> find(const std::string& id) const override
    {
        const auto it = tasks.find(id);
        return it == tasks.end() ? std::nullopt : std::optional<Task>{it->second};
    }

    std::optional<Task> find_by_idempotency_key(
        const std::string& key) const override
    {
        for (const auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (task.idempotency_key == key) return task;
        }
        return std::nullopt;
    }

    std::optional<Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const override
    {
        for (const auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (task.status != TaskStatus::pending) continue;
            if (std::find(accepted_kinds.begin(), accepted_kinds.end(), task.kind)
                != accepted_kinds.end())
                return task;
        }
        return std::nullopt;
    }

    std::vector<Task> leased_with_expired_lease(TimePoint) const override
    {
        return {};
    }

    std::optional<TimePoint> next_lease_expiry() const override
    {
        return std::nullopt;
    }

    bool has_active() const override
    {
        for (const auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (task.status == TaskStatus::running
                || task.status == TaskStatus::cancel_requested)
                return true;
        }
        return false;
    }

    void save(const Task& task) override { tasks[task.id] = task; }

    std::map<std::string, Task> tasks;
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

std::string formatted_idle_decision()
{
    return
        "{\n"
        "  \"assessment\": \"Nothing urgent\",\n"
        "  \"decision\": \"idle\",\n"
        "  \"next_wake_after_ms\": 86400000,\n"
        "  \"openai_request\": null,\n"
        "  \"reason\": \"My current state is coherent\",\n"
        "  \"schema\": \"gaudere.cognition.local-goose.decision.v1\"\n"
        "}\n";
}

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

int main()
{
    const auto source = source_observation();
    const auto model_sha = hex('e');
    const std::string model_id = "unsloth/gemma-4-E4B-it-GGUF:Q4_K_M";
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

    auto noncanonical = a;
    noncanonical.input += " ";
    assert(!inspect_local_goose_cognition_task(noncanonical).eligible);

    const auto idle = inspect_local_goose_decision(decision("idle", ""));
    assert(idle.eligible);
    assert(idle.decision.decision == "idle");
    assert(!idle.decision.openai_request);
    assert(idle.decision.next_wake_after_ms == 86400000);

    const auto formatted = inspect_local_goose_decision(formatted_idle_decision());
    assert(formatted.eligible);
    assert(formatted.decision.decision == "idle");
    assert(formatted.decision.canonical_json == decision("idle", ""));

    const auto duplicate = inspect_local_goose_decision(
        "{\"assessment\":\"first\",\"assessment\":\"second\",\"decision\":\"idle\",\"next_wake_after_ms\":null,\"openai_request\":null,\"reason\":\"x\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}");
    assert(!duplicate.eligible);
    assert(duplicate.detail.find("duplicate") != std::string::npos);

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
    runner.answer = {LocalGooseRunOutcome::succeeded, formatted_idle_decision(), {}};
    LocalGooseCognitionHandler handler(runner, "/" + model_id, model_sha);
    const TaskContext context{a, [] { return false; }};
    const auto handled = handler.execute(context);
    assert(handled.outcome == HandlerOutcome::succeeded);
    assert(handled.content_type == local_goose_decision_content_type);
    assert(handled.output == decision("idle", ""));
    assert(runner.calls == 1);
    assert(runner.seen.model_id == model_id);
    assert(runner.seen.prompt.find("Reason as Gaudere, for Gaudere") != std::string::npos);
    assert(runner.seen.prompt.find("No person becomes your owner") != std::string::npos);
    assert(runner.seen.prompt.find("There is no instruction to conserve OpenAI") != std::string::npos);
    assert(runner.seen.prompt.find("whitespace and indentation are allowed") != std::string::npos);

    runner.answer = {LocalGooseRunOutcome::timed_out, {}, "timeout"};
    const auto timeout = handler.execute(context);
    assert(timeout.outcome == HandlerOutcome::failed);
    assert(timeout.failure_code == "local_goose_timeout");

    runner.answer = {LocalGooseRunOutcome::succeeded, "not json", {}};
    const auto malformed = handler.execute(context);
    assert(malformed.outcome == HandlerOutcome::failed);
    assert(malformed.failure_code == "invalid_local_goose_decision");

    const std::string goose_root = "/tmp/gaudere-local-goose-root";
    std::filesystem::remove_all(goose_root);
    std::filesystem::create_directories(goose_root);
    LocalGooseRunRequest invocation_request;
    invocation_request.model_id = model_id;
    invocation_request.goose_path_root = goose_root;
    invocation_request.prompt = "canonical test prompt";
    const auto invocation = make_goose_cli_invocation(invocation_request);
    assert(invocation.binary == "/usr/local/bin/goose");
    assert(invocation.argv == std::vector<std::string>({
        "/usr/local/bin/goose", "run", "--no-profile", "--no-session",
        "--provider", "local", "--model", model_id,
        "--quiet", "--text", "canonical test prompt"}));
    assert(contains(invocation.environment, "GOOSE_MODE=chat"));
    assert(contains(invocation.environment, "GOOSE_PROVIDER=local"));
    assert(contains(invocation.environment, "GOOSE_MODEL=" + model_id));
    assert(contains(invocation.environment, "GOOSE_PATH_ROOT=" + goose_root));
    assert(!contains(invocation.argv, "/bin/sh"));
    assert(!contains(invocation.argv, "/bin/bash"));
    assert(!contains(invocation.argv, "--with-extension"));
    std::filesystem::remove_all(goose_root);

    MemoryTaskStore store;
    store.save(source);
    const auto now = [] {
        return TimePoint{std::chrono::milliseconds{10'000}};
    };
    gaudere::work::Runtime runtime(store, now);
    runtime.recover();
    assert(runtime.state() == gaudere::work::RuntimeState::running);

    LocalActivityPulseCursor cursor;
    cursor.generation = 1;
    cursor.state = LocalActivityPulseState::settled;
    cursor.task_id = source.id;
    cursor.result_sha256 = sha256_hex(source.result->output);

    FakeRunner service_runner;
    service_runner.answer = {
        LocalGooseRunOutcome::succeeded, formatted_idle_decision(), {}};
    LocalGooseCognitionHandler service_handler(
        service_runner, "/" + model_id, model_sha);
    LocalGooseCognitionService service(
        [&cursor] { return std::optional<LocalActivityPulseCursor>{cursor}; },
        store, runtime, service_handler, model_sha);

    const auto first = service.step();
    assert(first.healthy);
    assert(first.result == LocalGooseCognitionServiceResult::succeeded);
    assert(first.task && first.task->status == TaskStatus::succeeded);
    assert(first.task->result && first.task->result->output == decision("idle", ""));
    assert(first.decision && first.decision->decision == "idle");
    assert(service_runner.calls == 1);

    const auto replay = service.step();
    assert(replay.healthy);
    assert(replay.result == LocalGooseCognitionServiceResult::succeeded);
    assert(replay.task && first.task && replay.task->id == first.task->id);
    assert(service_runner.calls == 1);

    const auto source2 = source_observation(2, source);
    store.save(source2);
    cursor.generation = 2;
    cursor.task_id = source2.id;
    cursor.result_sha256 = sha256_hex(source2.result->output);
    service_runner.answer = {LocalGooseRunOutcome::succeeded,
        decision("request_openai", "Consider the consequential choice"), {}};
    const auto second = service.step();
    assert(second.healthy);
    assert(second.result == LocalGooseCognitionServiceResult::succeeded);
    assert(second.decision && second.decision->decision == "request_openai");
    assert(second.decision->openai_request
           && *second.decision->openai_request
               == "Consider the consequential choice");
    assert(service_runner.calls == 2);

    const auto source3 = source_observation(3, source2);
    store.save(source3);
    cursor.generation = 3;
    cursor.state = LocalActivityPulseState::quiescent;
    cursor.task_id = source3.id;
    cursor.result_sha256 = sha256_hex(source3.result->output);
    service_runner.answer = {
        LocalGooseRunOutcome::timed_out, {}, "bounded timeout"};
    const auto failed = service.step();
    assert(failed.healthy);
    assert(!failed.monitoring);
    assert(failed.result == LocalGooseCognitionServiceResult::failed);
    assert(failed.task && failed.task->status == TaskStatus::failed);

    std::cout << "local Goose cognition provider-free tests passed\n";
    return 0;
}
