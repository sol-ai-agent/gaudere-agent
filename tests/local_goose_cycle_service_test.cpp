#include "LocalGooseCycle.hpp"
#include "LocalGooseCycleHandler.hpp"
#include "LocalGooseCycleSchedulerBridge.hpp"
#include "LocalGooseCycleService.hpp"
#include "LocalGooseCycleStore.hpp"
#include "LocalGooseRunner.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using TimePoint = gaudere::work::TimePoint;

std::string hex(const char c) { return std::string(64, c); }

Task observation(const std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1'000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 1;
    facts.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('b');
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
        "provider.call:openai.responses:cognition.current.v0:" + hex('c');
    facts.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('d');
    facts.historical_wake_scope = "cognition.reflect.wake.v0";
    facts.historical_wake_sha256 = hex('e');

    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{
        local_continuity_observation_content_type, task.input, {}, {}};
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
        for (const auto& entry : tasks)
            if (entry.second.idempotency_key == key) return entry.second;
        return std::nullopt;
    }

    std::optional<Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const override
    {
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if (task.status == TaskStatus::pending
                && std::find(accepted_kinds.begin(), accepted_kinds.end(), task.kind)
                    != accepted_kinds.end())
                return task;
        }
        return std::nullopt;
    }

    std::vector<Task> leased_with_expired_lease(const TimePoint now) const override
    {
        std::vector<Task> expired;
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if (task.lease && task.lease->expires_at <= now)
                expired.push_back(task);
        }
        return expired;
    }

    std::optional<TimePoint> next_lease_expiry() const override
    {
        std::optional<TimePoint> next;
        for (const auto& entry : tasks) {
            const auto& task = entry.second;
            if (!task.lease) continue;
            if (!next || task.lease->expires_at < *next)
                next = task.lease->expires_at;
        }
        return next;
    }

    bool has_active() const override
    {
        for (const auto& entry : tasks) {
            const auto status = entry.second.status;
            if (status == TaskStatus::running
                || status == TaskStatus::cancel_requested)
                return true;
        }
        return false;
    }

    void save(const Task& task) override { tasks[task.id] = task; }

    std::map<std::string, Task> tasks;
};

std::string continue_decision()
{
    return
        "{\"assessment\":\"Continue locally\",\"decision\":\"continue_local\","
        "\"next_wake_after_ms\":null,\"openai_request\":null,"
        "\"reason\":\"Another local pass is useful\","
        "\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}";
}

std::string temp_path()
{
    return std::string{"/tmp/gaudere-local-goose-cycle-service-"}
        + std::to_string(static_cast<long long>(getpid())) + ".db";
}

std::int64_t milliseconds(
    const gaudere::scheduling::wake::Scheduler::TimePoint point)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        point.time_since_epoch()).count();
}

} // namespace

int main()
{
    const auto first_observation = observation(1);
    const auto second_observation = observation(2, first_observation);
    const auto anchor = observation(3, second_observation);
    const auto model_sha = hex('f');
    const std::string model_id = "test/local-goose-cycle";

    MemoryTaskStore task_store;
    task_store.save(anchor);

    std::int64_t now_ms = 9'000;
    gaudere::work::Runtime runtime(task_store, [&now_ms] {
        return TimePoint{std::chrono::milliseconds{now_ms}};
    });
    runtime.recover();

    FakeRunner runner;
    runner.answer = {
        LocalGooseRunOutcome::succeeded, continue_decision(), {}};
    LocalGooseCycleHandler handler(runner, "/" + model_id, model_sha);

    const auto path = temp_path();
    std::remove(path.c_str());
    LocalGooseCycleStore cycle_store(path);

    LocalGooseCycleCursor seed;
    seed.anchor_observation_task_id = anchor.id;
    seed.anchor_observation_result_sha256 = sha256_hex(anchor.result->output);
    assert(valid_local_goose_cycle_cursor(seed));
    assert(cycle_store.seed(seed).result == LocalGooseCycleStoreResult::accepted);

    LocalGooseCycleService dormant_service(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto dormant = dormant_service.step();
    assert(dormant.healthy && !dormant.active);
    assert(dormant.result == LocalGooseCycleServiceResult::dormant);
    assert(runner.calls == 0);

    LocalGooseCycleCursor scheduled = seed;
    scheduled.revision = 1;
    scheduled.generation = 1;
    scheduled.state = LocalGooseCycleState::scheduled;
    scheduled.due_at_ms = 10'000;
    assert(valid_local_goose_cycle_transition(seed, scheduled));
    assert(cycle_store.replace(seed, scheduled).result
           == LocalGooseCycleStoreResult::accepted);

    gaudere::scheduling::wake::Scheduler scheduler;
    LocalGooseCycleSchedulerBridge scheduler_bridge(scheduler);
    assert(scheduler_bridge.arm(scheduled)
           == LocalGooseCycleSchedulerArmResult::scheduled);
    assert(scheduler.next() && milliseconds(*scheduler.next()) == 10'000);
    assert(inspect_local_goose_cycle_deadline(seed).eligible);
    assert(!inspect_local_goose_cycle_deadline(seed).active);

    LocalGooseCycleService service_before_due(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto waiting = service_before_due.step();
    assert(waiting.healthy && waiting.active);
    assert(waiting.result == LocalGooseCycleServiceResult::waiting);
    assert(runner.calls == 0);

    now_ms = 10'000;
    LocalGooseCycleService service_capture(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto prepared = service_capture.step();
    assert(prepared.healthy && prepared.active);
    assert(prepared.result == LocalGooseCycleServiceResult::prepared);
    assert(prepared.cursor
           && prepared.cursor->state == LocalGooseCycleState::prepared);
    assert(prepared.cursor->captured_at_ms == now_ms);
    assert(prepared.task
           && prepared.cursor->current_task_id == prepared.task->id);
    assert(!task_store.find(prepared.task->id));
    assert(runner.calls == 0);

    // Crash point 1: prepared cursor exists, Task does not. A new service must
    // reconstruct and submit the exact frozen Task, without re-capturing time.
    now_ms = 10'123;
    LocalGooseCycleService service_after_capture_crash(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto submitted = service_after_capture_crash.step();
    assert(submitted.healthy && submitted.active);
    assert(submitted.result == LocalGooseCycleServiceResult::submitted);
    assert(submitted.task && prepared.task
           && submitted.task->id == prepared.task->id);
    assert(runner.calls == 0);

    // Crash point 2: submitted pending Task exists. A new service executes it once.
    LocalGooseCycleService service_after_submit_crash(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto executed = service_after_submit_crash.step();
    assert(executed.healthy && executed.active);
    assert(executed.result == LocalGooseCycleServiceResult::executed);
    assert(executed.task && executed.task->status == TaskStatus::succeeded);
    assert(runner.calls == 1);
    assert(runner.seen.model_id == model_id);
    assert(runner.seen.prompt.find("provider-free local cognition cycle")
           != std::string::npos);

    const auto settled = service_after_submit_crash.step();
    assert(settled.healthy && settled.active);
    assert(settled.result == LocalGooseCycleServiceResult::scheduled);
    assert(settled.cursor
           && settled.cursor->state == LocalGooseCycleState::scheduled);
    assert(settled.cursor->generation == 2);
    assert(settled.cursor->predecessor_task_id == executed.task->id);
    assert(settled.cursor->predecessor_result_sha256
           == sha256_hex(executed.task->result->output));
    assert(settled.cursor->due_at_ms
           == now_ms + local_goose_cycle_minimum_delay_ms);
    assert(!settled.cursor->captured_at_ms);
    assert(settled.cursor->current_task_id.empty());

    const auto gen2_deadline = inspect_local_goose_cycle_deadline(settled.cursor);
    assert(gen2_deadline.eligible && gen2_deadline.active
           && gen2_deadline.deadline
           && milliseconds(*gen2_deadline.deadline) == *settled.cursor->due_at_ms);

    now_ms = *settled.cursor->due_at_ms;
    const auto gen2_prepared = service_after_submit_crash.step();
    assert(gen2_prepared.result == LocalGooseCycleServiceResult::prepared);
    assert(gen2_prepared.task);
    const auto gen2_inspection = inspect_local_goose_cycle_task(*gen2_prepared.task);
    assert(gen2_inspection.eligible && gen2_inspection.generation == 2);
    assert(gen2_inspection.predecessor);
    assert(gen2_inspection.predecessor->task_id == executed.task->id);
    assert(gen2_inspection.predecessor->result_sha256
           == sha256_hex(executed.task->result->output));

    const auto gen2_submitted = service_after_submit_crash.step();
    assert(gen2_submitted.result == LocalGooseCycleServiceResult::submitted);
    assert(gen2_submitted.task && gen2_submitted.task->status == TaskStatus::pending);

    // Crash point 3: execution lease is durable. Generic Work recovery, not the
    // cycle service, returns the exact Task to pending after lease expiry.
    assert(runtime.start(gen2_submitted.task->id, "synthetic-crashed-worker"));
    const auto running = task_store.find(gen2_submitted.task->id);
    assert(running && running->status == TaskStatus::running && running->lease);
    now_ms += running->limits.max_runtime.count() + 1;
    assert(runtime.recover_expired() == 1);
    const auto recovered = task_store.find(gen2_submitted.task->id);
    assert(recovered && recovered->status == TaskStatus::pending);
    assert(recovered->attempts_started == 1);

    runner.answer = {LocalGooseRunOutcome::failed, {}, "synthetic failure"};
    LocalGooseCycleService service_after_lease_crash(
        cycle_store, task_store, runtime, handler, model_sha,
        [&now_ms] { return now_ms; });
    const auto failed_execution = service_after_lease_crash.step();
    assert(failed_execution.result == LocalGooseCycleServiceResult::executed);
    assert(failed_execution.task
           && failed_execution.task->status == TaskStatus::failed);
    assert(failed_execution.task->attempts_started == 2);
    assert(runner.calls == 2);

    const auto blocked = service_after_lease_crash.step();
    assert(blocked.healthy && !blocked.active);
    assert(blocked.result == LocalGooseCycleServiceResult::blocked);
    assert(blocked.cursor
           && blocked.cursor->state == LocalGooseCycleState::blocked);
    assert(blocked.cursor->generation == gen2_prepared.cursor->generation);
    assert(blocked.cursor->predecessor_task_id
           == gen2_prepared.cursor->predecessor_task_id);
    assert(blocked.cursor->due_at_ms == gen2_prepared.cursor->due_at_ms);
    assert(blocked.cursor->captured_at_ms
           == gen2_prepared.cursor->captured_at_ms);
    assert(blocked.cursor->current_task_id
           == gen2_prepared.cursor->current_task_id);

    const auto no_replay = service_after_lease_crash.step();
    assert(no_replay.result == LocalGooseCycleServiceResult::blocked);
    assert(runner.calls == 2);
    assert(!inspect_local_goose_cycle_deadline(no_replay.cursor).active);

    std::remove(path.c_str());
    std::cout << "local Goose cycle scheduler/service: ok\n";
    return 0;
}
