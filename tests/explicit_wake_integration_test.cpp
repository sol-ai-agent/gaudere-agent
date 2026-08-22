#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "LiveControlProcessor.hpp"
#include "OpenAIBudget.hpp"
#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"
#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;

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
            / ("gaudere-explicit-wake-integration-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + ".db");
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

gaudere::work::Task synthetic_source()
{
    gaudere::work::Task task;
    task.id = "synthetic-proposal";
    task.idempotency_key = "cognition.reflect.v1:synthetic-proposal";
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "offline synthetic provider-free fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type,
        "{\"decision\":\"propose_wake\","
        "\"reason\":\"One inert proof event.\","
        "\"schema\":\"gaudere.cognition.decision.v1\","
        "\"wake_after_seconds\":900}",
        {}, {}};
    return task;
}

std::int64_t count_rows(const std::filesystem::path& path, const char* table)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("cannot open disposable integration database");
    }
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
            != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("cannot count disposable integration rows");
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

void test_explicit_source_to_inert_fire_end_to_end()
{
    TemporaryDatabase database;
    gaudere::work::TimePoint fake_now =
        std::chrono::floor<std::chrono::milliseconds>(
            std::chrono::system_clock::now()) - 900s + 80ms;
    const auto clock = [&fake_now] { return fake_now; };

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::work::Runtime work_runtime(tasks, clock);
    gaudere::scheduling::wake::WakeIntentRuntime wake_runtime(
        wakes, clock, explicit_wake_scope, {explicit_wake_max_total});
    ExplicitWake explicit_wake(tasks, wake_runtime);
    LiveControlProcessor processor(
        work_runtime, tasks, budgets, openai_bootstrap_budget_policy(), false,
        &explicit_wake);
    TaskExecutor executor(work_runtime, tasks);
    TaskDispatcher dispatcher(tasks, executor);
    gaudere::scheduling::wake::Scheduler scheduler;
    WorkController controller(
        scheduler, work_runtime, dispatcher, "integration-worker", &wake_runtime);
    LiveControlMailbox mailbox;

    tasks.save(synthetic_source());
    const auto budget_before = budgets.snapshot(
        std::string(openai_budget_scope()), fake_now,
        openai_bootstrap_budget_policy());
    work_runtime.recover();
    expect(controller.start(), "integration worker starts");
    expect(controller.wait_and_run() == WorkCycleResult::idle,
           "startup reaches true idle with no polling deadline");
    expect(!scheduler.next(), "no wake exists before explicit acceptance");

    auto accept = mailbox.submit(
        LiveControlCommand{LiveControlOperation::accept_wake,
                           "synthetic-proposal", {}});
    const auto processed = processor.process(mailbox);
    const auto accepted_reply = accept->wait();
    expect(processed.wake_deadline_may_have_changed
               && !processed.work_may_be_pending && accepted_reply.ok,
           "main-worker processor accepts the canonical source without work");
    controller.refresh_deadlines();

    const auto durable = explicit_wake.find("synthetic-proposal");
    expect(durable && scheduler.next() == durable->due_at,
           "acceptance commit precedes exact in-memory deadline arming");
    expect(count_rows(database.path, "tasks") == 1
               && count_rows(database.path, "wake_intents") == 1,
           "acceptance adds one wake and no successor Task");

    if (durable) {
        fake_now = durable->due_at;
    }
    expect(controller.wait_and_run() == WorkCycleResult::idle,
           "exact scheduler event remains inert normal worker activity");

    auto inspect = mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_wake,
                           "synthetic-proposal", {}});
    static_cast<void>(processor.process(mailbox));
    const auto fired_reply = inspect->wait();
    expect(fired_reply.ok
               && fired_reply.body.find("status=fired") != std::string::npos,
           "fired state is durable and observable through live control");
    expect(!scheduler.next(), "fire creates no automatic successor deadline");
    expect(count_rows(database.path, "tasks") == 1,
           "fire creates zero successor Tasks");

    const auto budget_after = budgets.snapshot(
        std::string(openai_budget_scope()), fake_now,
        openai_bootstrap_budget_policy());
    expect(budget_before.total_used == 0 && budget_after.total_used == 0,
           "complete accept-to-fire lifecycle consumes zero provider permits");

    controller.stop();
    expect(controller.wait_and_run() == WorkCycleResult::stopped,
           "integration worker shuts down through normal draining path");
    expect(work_runtime.try_mark_safe(),
           "provider-free lifecycle is safe after clean shutdown");
}

} // namespace

int main()
{
    test_explicit_source_to_inert_fire_end_to_end();
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All explicit wake integration tests passed\n";
    return 0;
}
