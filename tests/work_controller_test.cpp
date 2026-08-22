#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

using gaudere::work::Runtime;
using gaudere::work::SubmitResult;
using gaudere::work::Task;
using gaudere::work::TaskStatus;
using SqliteStore = gaudere::persistence::sqlite::TaskStore;
using WakeSqliteStore = gaudere::persistence::sqlite::WakeIntentStore;
using WakeRuntime = gaudere::scheduling::wake::WakeIntentRuntime;
using Scheduler = gaudere::scheduling::wake::Scheduler;
using namespace std::chrono_literals;
using namespace gaudere_agent;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-agent-controller-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    }

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

Task make_task(std::string id, std::string kind, std::string input)
{
    Task task;
    task.id = std::move(id);
    task.idempotency_key = task.id + "-key";
    task.kind = std::move(kind);
    task.input_content_type = "text/plain";
    task.input = std::move(input);
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 1024;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    return task;
}

class EchoHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        return HandlerResult{HandlerOutcome::succeeded, "text/plain",
                             context.task.input, {}, {}};
    }
};

class BlockingHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        entered.store(true);
        while (!context.cancellation_requested()) {
            std::this_thread::sleep_for(1ms);
        }
        return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
    }

    std::atomic_bool entered{false};
};

struct Harness {
    explicit Harness(const std::filesystem::path& path)
        : store(path.string()),
          wake_store(path.string()),
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          wake_runtime(wake_store, [] { return std::chrono::system_clock::now(); },
                       "test.wake", {3}),
          executor(runtime, store),
          dispatcher(store, executor),
          controller(scheduler, runtime, dispatcher, "local-worker", &wake_runtime)
    {
        runtime.recover();
        expect(dispatcher.register_handler("local.echo", echo),
               "local echo handler registration succeeds");
        expect(dispatcher.register_handler("local.blocking", blocking),
               "blocking handler registration succeeds");
    }

    SqliteStore store;
    WakeSqliteStore wake_store;
    Runtime runtime;
    WakeRuntime wake_runtime;
    TaskExecutor executor;
    TaskDispatcher dispatcher;
    Scheduler scheduler;
    EchoHandler echo;
    BlockingHandler blocking;
    WorkController controller;
};

void test_initial_wake_dispatches_existing_work()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.runtime.submit(make_task("startup", "local.echo", "hello"))
               == SubmitResult::accepted,
           "startup task is submitted before controller start");
    expect(harness.controller.start(), "controller starts once");
    expect(!harness.controller.start(), "controller cannot start twice");
    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "initial wake drains eligible startup work");
    const auto task = harness.store.find("startup");
    expect(task && task->status == TaskStatus::succeeded && task->result
               && task->result->output == "hello",
           "startup work completes durably");
}

void test_notification_advances_idle_controller()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.controller.start(), "idle controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "initial wake can become idle without polling");
    expect(!harness.scheduler.next(), "idle controller has no periodic wake");

    expect(harness.runtime.submit(make_task("notified", "local.echo", "wake"))
               == SubmitResult::accepted,
           "new work is durably submitted");
    harness.controller.notify_work();
    expect(harness.scheduler.next().has_value(),
           "accepted work notification schedules an immediate wake");
    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "notification wake dispatches new work");
    expect(harness.store.find("notified")->status == TaskStatus::succeeded,
           "notified work completes");
}

void test_future_lease_wakes_recovery_without_polling()
{
    TemporaryDatabase database;
    Harness harness(database.path);

    auto interrupted = make_task("interrupted", "local.echo", "recovered");
    interrupted.status = TaskStatus::running;
    interrupted.attempts_started = 1;
    interrupted.lease = gaudere::work::Lease{
        "dead-worker", std::chrono::system_clock::now() + 40ms};
    harness.store.save(interrupted);
    const auto durable = harness.store.find("interrupted");
    expect(durable && durable->lease,
           "interrupted lease is persisted before controller start");

    expect(harness.controller.start(), "controller starts with future lease");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "initial wake leaves an unexpired lease untouched");
    const auto deadline = harness.scheduler.next();
    expect(deadline && durable && durable->lease
               && *deadline == durable->lease->expires_at,
           "controller schedules the exact durable lease deadline");

    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "lease deadline wakes recovery and redispatches interrupted work");
    const auto done = harness.store.find("interrupted");
    expect(done && done->status == TaskStatus::succeeded
               && done->attempts_started == 2 && done->result
               && done->result->output == "recovered",
           "recovered work consumes the next bounded attempt and completes");
    expect(!harness.scheduler.next(),
           "completed recovery leaves no periodic wake behind");
}

void test_unknown_kind_remains_pending()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.runtime.submit(make_task("unknown", "provider.future", "later"))
               == SubmitResult::accepted,
           "unknown provider task is submitted durably");
    expect(harness.controller.start(), "controller starts with unknown work");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "unknown work does not cause a dispatch conflict");
    expect(harness.store.find("unknown")->status == TaskStatus::pending,
           "unknown kind remains pending for a future handler");
    expect(!harness.scheduler.next(),
           "unsupported pending work does not create a polling wake");
}

void test_exact_wake_fires_once_without_successor_work()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.wake_runtime.accept("wake", "source", 40ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "future durable wake is accepted before controller start");
    const auto durable = harness.wake_runtime.find("wake");
    expect(durable.has_value(), "future wake is durable before controller start");
    expect(harness.controller.start(), "controller starts with future wake");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "initial worker event leaves future wake scheduled");
    expect(durable && harness.scheduler.next() == durable->due_at,
           "controller arms the exact durable wake deadline");

    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "wake deadline causes one inert worker event");
    const auto fired = harness.wake_runtime.find("wake");
    expect(fired
               && fired->status
                    == gaudere::scheduling::wake::WakeIntentStatus::fired
               && fired->terminal_at && *fired->terminal_at >= fired->due_at,
           "sole worker records fired at the first safe due observation");
    expect(!harness.scheduler.next(),
           "fired wake creates no periodic or successor deadline");
}

void test_earliest_wake_precedes_task_lease_recovery()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    auto interrupted = make_task("lease-after-wake", "local.echo", "recovered");
    interrupted.status = TaskStatus::running;
    interrupted.attempts_started = 1;
    interrupted.lease = gaudere::work::Lease{
        "dead-worker", std::chrono::system_clock::now() + 100ms};
    harness.store.save(interrupted);
    expect(harness.wake_runtime.accept("wake-first", "source-first", 40ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "earlier wake is accepted beside a future task lease");
    const auto wake = harness.wake_runtime.find("wake-first");

    expect(harness.controller.start(), "combined-deadline controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "initial event preserves both future durable deadlines");
    expect(wake && harness.scheduler.next() == wake->due_at,
           "scheduler selects the earlier WakeIntent deadline");

    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "earlier wake fires without prematurely recovering the lease");
    const auto still_running = harness.store.find("lease-after-wake");
    expect(still_running && still_running->status == TaskStatus::running
               && still_running->lease
               && harness.scheduler.next() == still_running->lease->expires_at,
           "worker re-arms the remaining exact lease deadline after firing");

    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "remaining lease deadline later recovers normal work");
    expect(harness.store.find("lease-after-wake")->status
               == TaskStatus::succeeded,
           "wake observation creates no successor and does not block task recovery");
}

void test_clean_shutdown_rearms_future_wake_after_restart()
{
    TemporaryDatabase database;
    std::optional<gaudere::scheduling::wake::WakeIntentTimePoint> due_at;
    {
        Harness first(database.path);
        expect(first.wake_runtime.accept("restart", "restart-source", 2s)
                   == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
               "restart fixture wake is durably accepted");
        const auto durable = first.wake_runtime.find("restart");
        expect(durable.has_value(), "restart fixture is present in durable state");
        if (durable) {
            due_at = durable->due_at;
        }
        expect(first.controller.start(), "first controller starts");
        expect(first.controller.wait_and_run() == WorkCycleResult::idle,
               "first controller arms future wake without polling");
        expect(first.scheduler.next() == due_at,
               "first process owns the exact future deadline");
        first.controller.stop();
        expect(first.controller.wait_and_run() == WorkCycleResult::stopped,
               "clean shutdown stops the in-memory scheduler");
        expect(first.runtime.try_mark_safe(),
               "future inert wake does not make work runtime unsafe");
    }

    {
        Harness replacement(database.path);
        expect(replacement.controller.start(), "replacement controller starts");
        expect(replacement.controller.wait_and_run() == WorkCycleResult::idle,
               "replacement performs one startup reconciliation event");
        expect(replacement.scheduler.next() == due_at,
               "replacement re-arms the immutable durable deadline exactly");
        replacement.controller.stop();
        expect(replacement.controller.wait_and_run() == WorkCycleResult::stopped,
               "replacement can shut down with the future wake still durable");
    }
}

void test_revoked_armed_deadline_causes_at_most_one_inert_event()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.wake_runtime.accept("revoked", "revoked-source", 40ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "revocation fixture wake is accepted");
    const auto durable = harness.wake_runtime.find("revoked");
    expect(durable.has_value(), "revocation fixture is durable");
    const auto due_at = durable
        ? durable->due_at
        : gaudere::scheduling::wake::WakeIntentTimePoint{};
    expect(harness.controller.start(), "revocation controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "future revocation fixture is armed");
    expect(harness.wake_runtime.revoke("revoked", "operator")
               == gaudere::scheduling::wake::WakeIntentRevokeResult::revoked,
           "wake is durably revoked before due");
    harness.controller.refresh_deadlines();
    expect(durable && harness.scheduler.next() == due_at,
           "already-armed stale notification remains bounded to its deadline");

    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "stale deadline causes one inert worker event");
    expect(!harness.scheduler.next(),
           "revoked record is never re-armed after the stale event");
    const auto revoked = harness.wake_runtime.find("revoked");
    expect(revoked
               && revoked->status
                    == gaudere::scheduling::wake::WakeIntentStatus::revoked,
           "stale event cannot change terminal revocation");
}

void test_stop_prevents_new_dispatch()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.controller.start(), "controller starts before stop");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "controller reaches idle before stop");

    expect(harness.runtime.submit(make_task("late", "local.echo", "late"))
               == SubmitResult::accepted,
           "late task exists before shutdown begins");
    harness.controller.stop();
    expect(harness.runtime.state() == gaudere::work::RuntimeState::running,
           "cross-thread stop does not mutate runtime state directly");
    harness.controller.notify_work();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "worker observes stop before any new dispatch");
    expect(harness.store.find("late")->status == TaskStatus::pending,
           "shutdown leaves not-yet-started work pending");
    expect(harness.runtime.state() == gaudere::work::RuntimeState::draining,
           "worker thread serializes the transition to draining");
    expect(harness.runtime.try_mark_safe(),
           "draining runtime becomes safe when no task was started");
}

void test_stop_cancels_cooperative_running_handler()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    expect(harness.runtime.submit(
               make_task("blocking", "local.blocking", "wait"))
               == SubmitResult::accepted,
           "blocking task is submitted");
    expect(harness.controller.start(), "controller starts for blocking task");

    std::thread stopper([&] {
        while (!harness.blocking.entered.load()) {
            std::this_thread::sleep_for(1ms);
        }
        harness.controller.stop();
    });

    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "worker drains after cooperative handler observes stop");
    stopper.join();

    const auto task = harness.store.find("blocking");
    expect(task && task->status == TaskStatus::cancelled && task->result
               && task->result->failure_code == "cancelled"
               && task->cancel_reason == "worker shutdown requested",
           "running handler stop is acknowledged durably as cancellation");
    expect(harness.runtime.state() == gaudere::work::RuntimeState::draining,
           "runtime enters draining only after handler cancellation completes");
    expect(harness.runtime.try_mark_safe(),
           "cancelled running handler leaves runtime safe to stop");
}

} // namespace

int main()
{
    test_initial_wake_dispatches_existing_work();
    test_notification_advances_idle_controller();
    test_future_lease_wakes_recovery_without_polling();
    test_unknown_kind_remains_pending();
    test_exact_wake_fires_once_without_successor_work();
    test_earliest_wake_precedes_task_lease_recovery();
    test_clean_shutdown_rearms_future_wake_after_restart();
    test_revoked_armed_deadline_causes_at_most_one_inert_event();
    test_stop_prevents_new_dispatch();
    test_stop_cancels_cooperative_running_handler();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All work controller tests passed\n";
    return 0;
}
