#include "TaskDispatcher.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
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
            / ("gaudere-agent-dispatcher-test-" + std::to_string(
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

void test_handler_registration()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    TaskExecutor executor(runtime, store);
    TaskDispatcher dispatcher(store, executor);
    EchoHandler echo;

    expect(!dispatcher.register_handler("", echo),
           "empty task kind cannot be registered");
    expect(dispatcher.register_handler("local.echo", echo),
           "first handler registration succeeds");
    expect(!dispatcher.register_handler("local.echo", echo),
           "duplicate handler registration is rejected");
}

void test_filtered_fifo_dispatch()
{
    TemporaryDatabase database;
    SqliteStore store(database.path.string());
    Runtime runtime(store, [] { return gaudere::work::TimePoint{}; });
    runtime.recover();
    TaskExecutor executor(runtime, store);
    TaskDispatcher dispatcher(store, executor);
    EchoHandler echo;

    expect(runtime.submit(make_task("unsupported", "provider.missing", "later"))
               == SubmitResult::accepted,
           "unsupported task is durably submitted");
    expect(runtime.submit(make_task("z-first", "local.echo", "first"))
               == SubmitResult::accepted,
           "first supported task is submitted");
    expect(runtime.submit(make_task("a-second", "local.echo", "second"))
               == SubmitResult::accepted,
           "second supported task is submitted");

    expect(dispatcher.dispatch_one("worker") == DispatchResult::idle,
           "dispatcher with no handlers leaves all tasks untouched");
    expect(store.find("unsupported")->status == TaskStatus::pending
               && store.find("z-first")->status == TaskStatus::pending,
           "idle dispatch does not mutate pending work");

    expect(dispatcher.register_handler("local.echo", echo),
           "local echo handler is registered");
    expect(dispatcher.dispatch_one("") == DispatchResult::state_conflict,
           "empty worker identity is rejected before selection");

    expect(dispatcher.dispatch_one("worker") == DispatchResult::dispatched,
           "first supported task is dispatched");
    const auto first = store.find("z-first");
    expect(first && first->status == TaskStatus::succeeded && first->result
               && first->result->output == "first",
           "first supported task completes through TaskExecutor");
    expect(store.find("unsupported")->status == TaskStatus::pending,
           "unsupported earlier task remains pending without blocking dispatch");

    expect(dispatcher.dispatch_one("worker") == DispatchResult::dispatched,
           "second supported task is dispatched");
    const auto second = store.find("a-second");
    expect(second && second->status == TaskStatus::succeeded && second->result
               && second->result->output == "second",
           "supported tasks preserve SQLite insertion order");

    expect(dispatcher.dispatch_one("worker") == DispatchResult::idle,
           "dispatcher becomes idle when no registered kind is pending");
    expect(store.find("unsupported")->status == TaskStatus::pending,
           "unknown provider work remains available for a future handler");
}

} // namespace

int main()
{
    test_handler_registration();
    test_filtered_fifo_dispatch();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All task dispatcher tests passed\n";
    return 0;
}
