#include "OpenAIOneShot.hpp"

#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
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

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-openai-one-shot-test-" + std::to_string(
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

class DefiniteRejectHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        ++calls;
        last_input = context.task.input;
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "synthetic_rejection", "offline one-shot fixture"};
    }

    int calls = 0;
    std::string last_input;
};

struct Harness {
    explicit Harness(const std::filesystem::path& path)
        : store(path.string()),
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          executor(runtime, store),
          dispatcher(store, executor),
          controller(scheduler, runtime, dispatcher, "one-shot-test-worker")
    {
        runtime.recover();
        if (!dispatcher.register_handler(openai_task_kind, handler)) {
            throw std::runtime_error("cannot register one-shot fixture handler");
        }
        if (!controller.start()) {
            throw std::runtime_error("cannot start one-shot fixture controller");
        }
    }

    gaudere::persistence::sqlite::TaskStore store;
    gaudere::work::Runtime runtime;
    gaudere::scheduling::wake::Scheduler scheduler;
    TaskExecutor executor;
    TaskDispatcher dispatcher;
    DefiniteRejectHandler handler;
    WorkController controller;
};

void test_one_shot_uses_durable_runtime_path()
{
    TemporaryDatabase database;
    Harness harness(database.path);

    run_openai_once(harness.runtime, harness.store, harness.controller,
                    "probe", "synthetic probe");

    const auto task = harness.store.find("probe");
    expect(task && task->status == gaudere::work::TaskStatus::failed,
           "one-shot reaches a durable terminal task state");
    expect(task && task->attempts_started == 1 && task->limits.max_attempts == 2,
           "one-shot uses the bounded OpenAI task attempt budget");
    expect(task && task->result
               && task->result->failure_code == "synthetic_rejection",
           "handler failure is stored durably through TaskExecutor");
    expect(harness.handler.calls == 1
               && harness.handler.last_input == "synthetic probe",
           "one-shot dispatches exactly one supported provider task");
    expect(harness.runtime.state() == gaudere::work::RuntimeState::draining,
           "one-shot stops WorkController through normal draining transition");
}

void test_terminal_duplicate_is_not_executed_again()
{
    TemporaryDatabase database;
    {
        Harness first(database.path);
        run_openai_once(first.runtime, first.store, first.controller,
                        "same-id", "first input");
        expect(first.handler.calls == 1, "first durable one-shot executes once");
    }

    Harness replacement(database.path);
    run_openai_once(replacement.runtime, replacement.store, replacement.controller,
                    "same-id", "different proposed input");
    const auto task = replacement.store.find("same-id");
    expect(replacement.handler.calls == 0,
           "terminal duplicate is reported without executing handler again");
    expect(task && task->result
               && task->result->failure_code == "synthetic_rejection"
               && task->input == "first input",
           "duplicate preserves the original durable task definition and result");
}

} // namespace

int main()
{
    test_one_shot_uses_durable_runtime_path();
    test_terminal_duplicate_is_not_executed_again();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI one-shot tests passed\n";
    return 0;
}
