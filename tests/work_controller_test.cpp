#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

using gaudere::work::Runtime;
using gaudere::work::SubmitResult;
using gaudere::work::Task;
using gaudere::work::TaskStatus;
using SqliteStore = gaudere::persistence::sqlite::TaskStore;
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
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          executor(runtime, store),
          dispatcher(store, executor),
          controller(scheduler, runtime, dispatcher, "local-worker")
    {
        runtime.recover();
        expect(dispatcher.register_handler("local.echo", echo),
               "local echo handler registration succeeds");
        expect(dispatcher.register_handler("local.blocking", blocking),
               "blocking handler registration succeeds");
    }

    SqliteStore store;
    Runtime runtime;
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
    test_stop_prevents_new_dispatch();
    test_stop_cancels_cooperative_running_handler();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All work controller tests passed\n";
    return 0;
}
