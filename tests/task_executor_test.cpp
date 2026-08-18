#include "TaskExecutor.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using gaudere::work::Runtime;
using gaudere::work::SubmitResult;
using gaudere::work::Task;
using gaudere::work::TaskStatus;
using SqliteStore = gaudere::persistence::sqlite::TaskStore;
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
            / ("gaudere-agent-executor-test-" + std::to_string(
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

Task make_task(std::string id, std::string input = "hello")
{
    Task task;
    task.id = id;
    task.idempotency_key = id + "-key";
    task.kind = "test.local";
    task.input_content_type = "text/plain";
    task.input = std::move(input);
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 1024;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 1;
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

class FailureHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext&) override
    {
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "local_rejected", "deterministic rejection"};
    }
};

class ThrowingHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext&) override
    {
        throw std::runtime_error("boom");
    }
};

class CancellingHandler final : public TaskHandler {
public:
    CancellingHandler(Runtime& runtime, std::string id)
        : runtime_(runtime), id_(std::move(id))
    {
    }

    HandlerResult execute(const TaskContext& context) override
    {
        const bool requested = runtime_.request_cancel(id_, "test cancellation");
        expect(requested, "handler can trigger cooperative cancellation in test");
        expect(context.cancellation_requested(),
               "handler observes the durable cancellation request");
        return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
    }

private:
    Runtime& runtime_;
    std::string id_;
};

class WorkerStopHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        expect(context.cancellation_requested(),
               "handler observes the worker-stop cancellation probe");
        return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
    }
};

void test_success_round_trip()
{
    TemporaryDatabase database;
    {
        SqliteStore store(database.path.string());
        Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
        runtime.recover();
        expect(runtime.submit(make_task("echo")) == SubmitResult::accepted,
               "local echo task is submitted");
        TaskExecutor executor(runtime, store);
        EchoHandler handler;
        expect(executor.execute("echo", "local-test", handler)
                   == ExecuteResult::completed,
               "local echo handler completes");
        const auto done = store.find("echo");
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->output == "hello",
               "handler output is persisted through the bounded runtime");
    }

    SqliteStore reopened(database.path.string());
    const auto durable = reopened.find("echo");
    expect(durable && durable->status == TaskStatus::succeeded
               && durable->result && durable->result->output == "hello",
           "completed local execution survives SQLite reopen");
}

void test_output_limit()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    auto task = make_task("bounded", "hello");
    task.limits.max_output_bytes = 3;
    expect(runtime.submit(task) == SubmitResult::accepted,
           "bounded-output task is submitted");
    TaskExecutor executor(runtime, store);
    EchoHandler handler;
    expect(executor.execute("bounded", "local-test", handler)
               == ExecuteResult::completed,
           "oversized local output completes as a durable bounded failure");
    const auto done = store.find("bounded");
    expect(done && done->status == TaskStatus::failed && done->result
               && done->result->failure_code == "output_limit_exceeded",
           "output limit is enforced after the handler boundary");
}

void test_explicit_failure()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(make_task("failure")) == SubmitResult::accepted,
           "failure task is submitted");
    TaskExecutor executor(runtime, store);
    FailureHandler handler;
    expect(executor.execute("failure", "local-test", handler)
               == ExecuteResult::completed,
           "explicit handler failure completes");
    const auto done = store.find("failure");
    expect(done && done->status == TaskStatus::failed && done->result
               && done->result->failure_code == "local_rejected",
           "explicit handler failure is durable");
}

void test_exception_requires_manual_review()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(make_task("throw")) == SubmitResult::accepted,
           "throwing task is submitted");
    TaskExecutor executor(runtime, store);
    ThrowingHandler handler;
    expect(executor.execute("throw", "local-test", handler)
               == ExecuteResult::completed,
           "handler exception is captured at the boundary");
    const auto done = store.find("throw");
    expect(done && done->status == TaskStatus::manual_review && done->result
               && done->result->failure_code == "handler_exception",
           "unknown handler effect is never blindly retried");
}

void test_cooperative_cancellation()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(make_task("cancel")) == SubmitResult::accepted,
           "cancellation task is submitted");
    TaskExecutor executor(runtime, store);
    CancellingHandler handler(runtime, "cancel");
    expect(executor.execute("cancel", "local-test", handler)
               == ExecuteResult::completed,
           "handler acknowledges cooperative cancellation");
    const auto done = store.find("cancel");
    expect(done && done->status == TaskStatus::cancelled && done->result
               && done->result->failure_code == "cancelled",
           "cooperative cancellation is durable");
}

void test_worker_stop_cancellation()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    expect(runtime.submit(make_task("worker-stop")) == SubmitResult::accepted,
           "worker-stop task is submitted");
    TaskExecutor executor(runtime, store);
    WorkerStopHandler handler;
    expect(executor.execute("worker-stop", "local-test", handler,
                            [] { return true; }) == ExecuteResult::completed,
           "worker-stop cancellation completes on the worker thread");
    const auto done = store.find("worker-stop");
    expect(done && done->status == TaskStatus::cancelled && done->result
               && done->result->failure_code == "cancelled"
               && done->cancel_reason == "worker shutdown requested",
           "worker stop becomes a durable acknowledged cancellation");
}

} // namespace

int main()
{
    test_success_round_trip();
    test_output_limit();
    test_explicit_failure();
    test_exception_requires_manual_review();
    test_cooperative_cancellation();
    test_worker_stop_cancellation();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All task executor tests passed\n";
    return 0;
}
