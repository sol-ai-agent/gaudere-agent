#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "LiveControlProcessor.hpp"
#include "OpenAIBudget.hpp"
#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"
#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <sqlite3.h>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using WakeIntentStatus = gaudere::scheduling::wake::WakeIntentStatus;
using WakeRuntime = gaudere::scheduling::wake::WakeIntentRuntime;
using WakeTimePoint = gaudere::scheduling::wake::WakeIntentTimePoint;

constexpr int uncommitted_crash_exit = 61;
constexpr int committed_crash_exit = 62;

int failures = 0;
std::atomic<unsigned long long> database_counter{0};

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    explicit TemporaryDatabase(std::string label)
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-agent-wake-adversarial-" + std::move(label) + "-"
               + std::to_string(::getpid()) + "-"
               + std::to_string(database_counter.fetch_add(1)) + ".db");
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

WakeTimePoint rounded_now()
{
    return std::chrono::floor<std::chrono::milliseconds>(
        std::chrono::system_clock::now());
}

Task task_definition(std::string id,
                     std::string kind,
                     std::string input)
{
    Task task;
    task.id = std::move(id);
    task.kind = std::move(kind);
    task.idempotency_key = task.kind + ":" + task.id;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = std::move(input);
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 2s;
    task.limits.max_attempts = 2;
    return task;
}

Task reflection_source(std::string id)
{
    auto task = task_definition(std::move(id), bounded_reflection_task_kind,
                                "offline adversarial source fixture");
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type,
        "{\"decision\":\"propose_wake\","
        "\"reason\":\"One inert recovery proof.\","
        "\"schema\":\"gaudere.cognition.decision.v1\","
        "\"wake_after_seconds\":900}",
        {}, {}};
    return task;
}

Task provider_history_task(const int index)
{
    const auto suffix = std::to_string(index);
    auto task = task_definition("historical-provider-" + suffix,
                                "provider.openai.responses",
                                "historical input " + suffix);
    task.idempotency_key = "provider.openai.responses:historical-" + suffix;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        "text/plain; charset=utf-8", "historical answer " + suffix, {}, {},
        "application/vnd.gaudere.provider-usage+json",
        "{\"cache_write_input_tokens\":0,\"cached_input_tokens\":0,"
        "\"input_tokens\":" + std::to_string(10 + index)
            + ",\"model\":\"gpt-5.6-sol\",\"output_tokens\":"
            + std::to_string(5 + index)
            + ",\"provider\":\"openai\",\"reasoning_tokens\":0,"
              "\"schema\":\"gaudere.provider_usage.v1\",\"total_tokens\":"
            + std::to_string(15 + (2 * index)) + "}"};
    return task;
}

gaudere::scheduling::wake::Action provider_history_action(const int index)
{
    gaudere::scheduling::wake::Action action;
    action.id = "historical-provider-action-" + std::to_string(index);
    action.idempotency_key = "provider.openai.responses:effect:"
        + std::to_string(index);
    action.critical = true;
    action.status = gaudere::scheduling::wake::ActionStatus::succeeded;
    action.effect_result = gaudere::scheduling::wake::EffectResult::confirmed;
    return action;
}

std::int64_t count_rows(const std::filesystem::path& path,
                        const std::string& table)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("cannot open disposable database for row count");
    }
    const std::string sql = "SELECT COUNT(*) FROM " + table;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
            != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("cannot count disposable database rows");
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::optional<int> read_wake_status(const std::filesystem::path& path,
                                    const std::string& scope,
                                    const std::string& id) noexcept
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        return std::nullopt;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT status FROM wake_intents WHERE scope=?1 AND id=?2",
            -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_bind_text(statement, 1, scope.c_str(), -1, SQLITE_TRANSIENT)
            != SQLITE_OK
        || sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT)
            != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return std::nullopt;
    }
    const int result = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::string table_snapshot(const std::filesystem::path& path,
                           const std::string& table)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("cannot open disposable database for snapshot");
    }
    const std::string sql = "SELECT * FROM " + table + " ORDER BY 1,2";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("cannot prepare disposable table snapshot");
    }

    std::string result;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) {
            break;
        }
        if (step != SQLITE_ROW) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("cannot read disposable table snapshot");
        }
        const int columns = sqlite3_column_count(statement);
        for (int column = 0; column < columns; ++column) {
            const int type = sqlite3_column_type(statement, column);
            result += std::to_string(type) + ":";
            if (type == SQLITE_INTEGER) {
                result += std::to_string(sqlite3_column_int64(statement, column));
            } else if (type == SQLITE_FLOAT) {
                result += std::to_string(sqlite3_column_double(statement, column));
            } else if (type == SQLITE_TEXT || type == SQLITE_BLOB) {
                const int bytes = sqlite3_column_bytes(statement, column);
                const auto* data = static_cast<const char*>(
                    sqlite3_column_blob(statement, column));
                result += std::to_string(bytes) + ":";
                if (data && bytes > 0) {
                    result.append(data, static_cast<std::size_t>(bytes));
                }
            }
            result.push_back('|');
        }
        result.push_back('\n');
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

int wait_for_child(const pid_t child) noexcept
{
    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

class EchoHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        return HandlerResult{HandlerOutcome::succeeded,
                             "text/plain; charset=utf-8",
                             context.task.input, {}, {}};
    }
};

class ReleaseHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext&) override
    {
        entered.store(true);
        while (!release.load()) {
            std::this_thread::sleep_for(1ms);
        }
        return HandlerResult{HandlerOutcome::succeeded,
                             "text/plain; charset=utf-8",
                             "released", {}, {}};
    }

    std::atomic_bool entered{false};
    std::atomic_bool release{false};
};

struct ControllerHarness {
    using Now = std::function<WakeTimePoint()>;

    ControllerHarness(const std::filesystem::path& path, Now now)
        : clock(std::move(now)),
          tasks(path.string()),
          wakes(path.string()),
          runtime(tasks, clock),
          wake_runtime(wakes, clock, "test.wake", {4}),
          executor(runtime, tasks),
          dispatcher(tasks, executor),
          controller(scheduler, runtime, dispatcher, "test-worker", &wake_runtime)
    {
        runtime.recover();
        expect(dispatcher.register_handler("local.echo", echo),
               "adversarial harness registers local echo");
        expect(dispatcher.register_handler("local.release", release),
               "adversarial harness registers release handler");
    }

    Now clock;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::Runtime runtime;
    WakeRuntime wake_runtime;
    TaskExecutor executor;
    TaskDispatcher dispatcher;
    gaudere::scheduling::wake::Scheduler scheduler;
    EchoHandler echo;
    ReleaseHandler release;
    WorkController controller;
};

struct ExplicitHarness {
    using Now = std::function<WakeTimePoint()>;

    ExplicitHarness(const std::filesystem::path& path, Now now)
        : clock(std::move(now)),
          tasks(path.string()),
          budgets(path.string()),
          wakes(path.string()),
          runtime(tasks, clock),
          wake_runtime(wakes, clock, explicit_wake_scope,
                       {explicit_wake_max_total}),
          explicit_wake(tasks, wake_runtime),
          processor(runtime, tasks, budgets, openai_bootstrap_budget_policy(),
                    false, &explicit_wake),
          executor(runtime, tasks),
          dispatcher(tasks, executor),
          controller(scheduler, runtime, dispatcher, "explicit-worker",
                     &wake_runtime)
    {
        runtime.recover();
    }

    Now clock;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::Runtime runtime;
    WakeRuntime wake_runtime;
    ExplicitWake explicit_wake;
    LiveControlProcessor processor;
    LiveControlMailbox mailbox;
    TaskExecutor executor;
    TaskDispatcher dispatcher;
    gaudere::scheduling::wake::Scheduler scheduler;
    WorkController controller;
};

void test_uncommitted_acceptance_rolls_back_after_hard_exit()
{
    TemporaryDatabase database("w06-before-commit");
    {
        gaudere::persistence::sqlite::WakeIntentStore schema(database.path.string());
    }

    const pid_t child = ::fork();
    if (child < 0) {
        expect(false, "W06 can fork the disposable crash fixture");
        return;
    }
    if (child == 0) {
        sqlite3* raw = nullptr;
        if (sqlite3_open_v2(database.path.c_str(), &raw,
                            SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK
            || sqlite3_exec(raw, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr)
                != SQLITE_OK
            || sqlite3_exec(
                   raw,
                   "INSERT INTO wake_intents "
                   "(scope,id,source_id,accepted_at_ms,due_at_ms,status,"
                   "terminal_at_ms,terminal_reason) VALUES "
                   "('test.wake','crash-before-commit','source-before',"
                   "1000,2000,0,NULL,'')",
                   nullptr, nullptr, nullptr) != SQLITE_OK) {
            ::_exit(60);
        }
        // Deliberately skip COMMIT, sqlite3_close(), and every C++ destructor.
        ::_exit(uncommitted_crash_exit);
    }

    expect(wait_for_child(child) == uncommitted_crash_exit,
           "W06 hard exit occurs with an uncommitted wake row");
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    WakeRuntime runtime(wakes, [] { return WakeTimePoint{1s}; },
                        "test.wake", {1});
    expect(!runtime.find("crash-before-commit"),
           "W06 process loss rolls back the partial acceptance completely");
    expect(runtime.accept("crash-before-commit", "source-before", 1s)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W06 the same explicit identity remains safely admissible after restart");
}

void test_committed_acceptance_rearms_after_hard_exit()
{
    TemporaryDatabase database("w07-after-commit");
    const WakeTimePoint fake_now = rounded_now();
    {
        gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
        tasks.save(reflection_source("crash-after-commit"));
        gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
        gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    }

    const pid_t child = ::fork();
    if (child < 0) {
        expect(false, "W07 can fork the disposable crash fixture");
        return;
    }
    if (child == 0) {
        try {
            gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
            gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
            gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
            gaudere::work::Runtime runtime(tasks, [fake_now] { return fake_now; });
            WakeRuntime wake_runtime(
                wakes, [fake_now] { return fake_now; }, explicit_wake_scope,
                {explicit_wake_max_total});
            ExplicitWake explicit_wake(tasks, wake_runtime);
            LiveControlProcessor processor(
                runtime, tasks, budgets, openai_bootstrap_budget_policy(), false,
                &explicit_wake);
            LiveControlMailbox mailbox;
            runtime.recover();
            static_cast<void>(mailbox.submit(LiveControlCommand{
                LiveControlOperation::accept_wake, "crash-after-commit", {}}));
            const auto processed = processor.process(mailbox);
            const auto durable = explicit_wake.find("crash-after-commit");
            if (!processed.wake_deadline_may_have_changed || !durable
                || durable->status != WakeIntentStatus::scheduled) {
                ::_exit(63);
            }
            // The worker has committed, but no caller has refreshed Scheduler and
            // no external socket reply exists in this disposable process.
            ::_exit(committed_crash_exit);
        } catch (...) {
            ::_exit(64);
        }
    }

    expect(wait_for_child(child) == committed_crash_exit,
           "W07 hard exit occurs after the durable commit and before arming");
    ExplicitHarness replacement(database.path, [fake_now] { return fake_now; });
    const auto durable = replacement.explicit_wake.find("crash-after-commit");
    expect(durable && durable->status == WakeIntentStatus::scheduled,
           "W07 restart finds the committed scheduled wake");
    expect(replacement.controller.start(),
           "W07 replacement controller starts normally");
    expect(replacement.controller.wait_and_run() == WorkCycleResult::idle,
           "W07 replacement performs one startup reconciliation event");
    expect(durable && replacement.scheduler.next() == durable->due_at,
           "W07 restart re-arms the immutable deadline exactly");
    const auto duplicate = replacement.explicit_wake.accept("crash-after-commit");
    expect(duplicate.result == ExplicitWakeAcceptResult::duplicate
               && duplicate.intent && durable
               && duplicate.intent->due_at == durable->due_at,
           "W07 retry after an uncertain reply is idempotent and preserves due_at");
    replacement.controller.stop();
    expect(replacement.controller.wait_and_run() == WorkCycleResult::stopped,
           "W07 replacement stops without firing the future wake");
    expect(replacement.runtime.try_mark_safe(),
           "W07 future inert wake leaves shutdown safe");
}

void test_forward_clock_jump_reconciles_on_scheduler_event()
{
    TemporaryDatabase database("w11-forward-clock");
    WakeTimePoint fake_now = rounded_now();
    ControllerHarness harness(database.path, [&fake_now] { return fake_now; });
    expect(harness.wake_runtime.accept("forward-jump", "forward-source", 5min)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W11 future wake is accepted before scheduler arming");
    const auto durable = harness.wake_runtime.find("forward-jump");
    expect(harness.controller.start(), "W11 controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W11 initial event arms the future durable deadline");
    expect(durable && harness.scheduler.next() == durable->due_at,
           "W11 scheduler holds the original future deadline");

    if (durable) {
        fake_now = durable->due_at + 1ms;
    }
    // A deterministic worker event models the scheduler returning after a forward
    // wall-clock discontinuity without mutating the host clock or adding polling.
    harness.controller.notify_work();
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W11 scheduler event observes the forward-jumped clock");
    const auto fired = harness.wake_runtime.find("forward-jump");
    expect(fired && fired->status == WakeIntentStatus::fired
               && fired->terminal_at && *fired->terminal_at == fake_now,
           "W11 all newly-due wake state fires once at the first worker observation");
    expect(!harness.scheduler.next(),
           "W11 forward reconciliation leaves no stale or polling deadline");
    expect(harness.wake_runtime.reconcile().fired == 0,
           "W11 repeated reconciliation cannot terminalize the wake twice");
    harness.controller.stop();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "W11 controller stops after the deterministic jump proof");
}

void test_queued_revoke_processed_at_due_fires()
{
    TemporaryDatabase database("w15-queued-revoke");
    WakeTimePoint fake_now = rounded_now();
    ExplicitHarness harness(database.path, [&fake_now] { return fake_now; });
    harness.tasks.save(reflection_source("queued-revoke"));

    auto accept = harness.mailbox.submit(LiveControlCommand{
        LiveControlOperation::accept_wake, "queued-revoke", {}});
    const auto accepted = harness.processor.process(harness.mailbox);
    const auto accept_reply = accept->wait();
    const auto durable = harness.explicit_wake.find("queued-revoke");
    expect(accepted.wake_deadline_may_have_changed && accept_reply.ok && durable,
           "W15 fixture acceptance commits through the worker mailbox");

    auto revoke = harness.mailbox.submit(LiveControlCommand{
        LiveControlOperation::revoke_wake, "queued-revoke",
        "queued strictly before due"});
    if (durable) {
        fake_now = durable->due_at;
    }
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = revoke->wait();
    const auto terminal = harness.explicit_wake.find("queued-revoke");
    expect(processed.wake_deadline_may_have_changed && reply.ok
               && reply.body.find("revocation=fired") != std::string::npos,
           "W15 dequeue at due deterministically gives firing precedence");
    expect(terminal && terminal->status == WakeIntentStatus::fired
               && terminal->terminal_reason.empty(),
           "W15 late processing cannot persist a pre-due revocation");
}

void test_mailbox_commit_order_survives_stop_and_restart()
{
    TemporaryDatabase database("w16-stop-order");
    WakeTimePoint fake_now = rounded_now();
    std::optional<WakeTimePoint> due_at;
    {
        ExplicitHarness first(database.path, [&fake_now] { return fake_now; });
        first.tasks.save(reflection_source("stop-order"));
        expect(first.controller.start(), "W16 first controller starts");
        expect(first.controller.wait_and_run() == WorkCycleResult::idle,
               "W16 first controller reaches idle before the race");

        auto accept = first.mailbox.submit(LiveControlCommand{
            LiveControlOperation::accept_wake, "stop-order", {}});
        first.controller.stop();
        const auto processed = first.processor.process(first.mailbox);
        const auto reply = accept->wait();
        const auto durable = first.explicit_wake.find("stop-order");
        if (durable) {
            due_at = durable->due_at;
        }
        expect(processed.wake_deadline_may_have_changed && reply.ok && durable
                   && durable->status == WakeIntentStatus::scheduled,
               "W16 sole worker commits queued acceptance in its observed order");
        expect(first.controller.wait_and_run() == WorkCycleResult::stopped,
               "W16 published stop then enters draining without dispatch");
        expect(first.runtime.try_mark_safe(),
               "W16 acceptance/stop ordering remains shutdown-safe");
    }

    {
        ExplicitHarness second(database.path, [&fake_now] { return fake_now; });
        expect(second.controller.start(), "W16 replacement controller starts");
        expect(second.controller.wait_and_run() == WorkCycleResult::idle,
               "W16 replacement reconciles the committed acceptance");
        expect(due_at && second.scheduler.next() == due_at,
               "W16 restart re-arms the one committed deadline exactly");

        auto revoke = second.mailbox.submit(LiveControlCommand{
            LiveControlOperation::revoke_wake, "stop-order",
            "operator shutdown ordering"});
        second.controller.stop();
        const auto processed = second.processor.process(second.mailbox);
        const auto reply = revoke->wait();
        const auto terminal = second.explicit_wake.find("stop-order");
        expect(processed.wake_deadline_may_have_changed && reply.ok && terminal
                   && terminal->status == WakeIntentStatus::revoked,
               "W16 queued revoke commits once before the worker enters draining");
        expect(second.controller.wait_and_run() == WorkCycleResult::stopped,
               "W16 second stop completes after the durable revoke ordering");
    }

    {
        ExplicitHarness third(database.path, [&fake_now] { return fake_now; });
        expect(third.controller.start(), "W16 terminal-state restart starts");
        expect(third.controller.wait_and_run() == WorkCycleResult::idle,
               "W16 terminal-state restart performs one inert startup event");
        expect(!third.scheduler.next(),
               "W16 revoked state never re-arms after restart");
        third.controller.stop();
        expect(third.controller.wait_and_run() == WorkCycleResult::stopped,
               "W16 terminal-state restart stops cleanly");
    }
}

void test_busy_worker_fires_once_after_due()
{
    TemporaryDatabase database("w17-busy-worker");
    ControllerHarness harness(
        database.path, [] { return std::chrono::system_clock::now(); });
    expect(harness.wake_runtime.accept("busy-wake", "busy-source", 300ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W17 wake is accepted before bounded work starts");
    const auto durable = harness.wake_runtime.find("busy-wake");
    expect(harness.runtime.submit(
               task_definition("busy-task", "local.release", "hold worker"))
               == gaudere::work::SubmitResult::accepted,
           "W17 bounded task is pending before controller start");

    std::optional<int> observed_status;
    std::atomic_bool observer_failed{false};
    std::thread observer([&] {
        const auto timeout = std::chrono::steady_clock::now() + 3s;
        while (!harness.release.entered.load()
               && std::chrono::steady_clock::now() < timeout) {
            std::this_thread::sleep_for(1ms);
        }
        if (!harness.release.entered.load() || !durable) {
            observer_failed.store(true);
            harness.release.release.store(true);
            return;
        }
        std::this_thread::sleep_until(durable->due_at + 20ms);
        observed_status = read_wake_status(
            database.path, "test.wake", "busy-wake");
        if (!observed_status) {
            observer_failed.store(true);
        }
        harness.release.release.store(true);
    });

    expect(harness.controller.start(), "W17 controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "W17 sole worker completes bounded work after crossing due_at");
    observer.join();
    expect(!observer_failed.load() && observed_status
               && *observed_status == static_cast<int>(WakeIntentStatus::scheduled),
           "W17 read-only observer sees no concurrent terminal mutation at due_at");
    const auto still_scheduled = harness.wake_runtime.find("busy-wake");
    expect(still_scheduled && still_scheduled->status == WakeIntentStatus::scheduled
               && durable && harness.scheduler.next() == durable->due_at,
           "W17 worker re-arms the overdue durable deadline after handler return");

    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W17 next worker observation terminalizes the overdue inert wake");
    const auto fired = harness.wake_runtime.find("busy-wake");
    expect(fired && fired->status == WakeIntentStatus::fired
               && fired->terminal_at && *fired->terminal_at >= fired->due_at,
           "W17 one late fired transition records measurable nonnegative lateness");
    expect(harness.wake_runtime.reconcile().fired == 0,
           "W17 later reconciliation cannot repeat the fired transition");
    expect(harness.tasks.find("busy-task")->status == TaskStatus::succeeded,
           "W17 ordinary bounded work completes independently of the wake");
    harness.controller.stop();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "W17 controller stops after the lateness proof");
}

void test_lease_earlier_than_wake_preserves_order()
{
    TemporaryDatabase database("w18-lease-first");
    ControllerHarness harness(
        database.path, [] { return std::chrono::system_clock::now(); });
    auto interrupted = task_definition(
        "lease-first-task", "local.echo", "recovered first");
    interrupted.status = TaskStatus::running;
    interrupted.attempts_started = 1;
    interrupted.lease = gaudere::work::Lease{
        "dead-worker", rounded_now() + 250ms};
    harness.tasks.save(interrupted);
    expect(harness.wake_runtime.accept("wake-second", "wake-second-source", 700ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W18 later wake coexists with an earlier task lease");
    const auto durable_task = harness.tasks.find("lease-first-task");
    const auto durable_wake = harness.wake_runtime.find("wake-second");

    expect(harness.controller.start(), "W18 lease-first controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W18 initial event preserves both future deadlines");
    expect(durable_task && durable_task->lease
               && harness.scheduler.next() == durable_task->lease->expires_at,
           "W18 scheduler selects the earlier lease deadline");
    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "W18 earlier lease recovers and dispatches normal work first");
    expect(harness.tasks.find("lease-first-task")->status == TaskStatus::succeeded
               && durable_wake && harness.scheduler.next() == durable_wake->due_at,
           "W18 remaining exact wake deadline is re-armed after lease recovery");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W18 later wake fires as an inert event");
    expect(harness.wake_runtime.find("wake-second")->status
               == WakeIntentStatus::fired,
           "W18 lease-first ordering does not lose the wake");
    harness.controller.stop();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "W18 lease-first controller stops cleanly");
}

void test_equal_lease_and_wake_deadlines_share_one_worker_event()
{
    TemporaryDatabase database("w18-equal-deadline");
    ControllerHarness harness(
        database.path, [] { return std::chrono::system_clock::now(); });
    expect(harness.wake_runtime.accept("equal-wake", "equal-source", 300ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W18 equal-deadline wake is accepted");
    const auto durable_wake = harness.wake_runtime.find("equal-wake");
    auto interrupted = task_definition(
        "equal-lease-task", "local.echo", "same event");
    interrupted.status = TaskStatus::running;
    interrupted.attempts_started = 1;
    interrupted.lease = gaudere::work::Lease{
        "dead-worker", durable_wake ? durable_wake->due_at : rounded_now() + 300ms};
    harness.tasks.save(interrupted);

    expect(harness.controller.start(), "W18 equal-deadline controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W18 initial event arms the shared exact deadline");
    expect(durable_wake && harness.scheduler.next() == durable_wake->due_at,
           "W18 one scheduler deadline represents equal durable events");
    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "W18 one worker event reconciles wake then recovers and dispatches lease");
    expect(harness.wake_runtime.find("equal-wake")->status
               == WakeIntentStatus::fired
               && harness.tasks.find("equal-lease-task")->status
                    == TaskStatus::succeeded,
           "W18 equal deadlines both complete exactly once in deterministic order");
    expect(!harness.scheduler.next(),
           "W18 shared event leaves no duplicate deadline");
    harness.controller.stop();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "W18 equal-deadline controller stops cleanly");
}

enum class ProductionTerminal { fire, revoke };

void run_production_like_history_fixture(const ProductionTerminal terminal)
{
    const std::string mode = terminal == ProductionTerminal::fire ? "fire" : "revoke";
    TemporaryDatabase database("w24-history-" + mode);
    WakeTimePoint fake_now = rounded_now();
    const auto policy = openai_bootstrap_budget_policy();
    const std::string scope(openai_budget_scope());
    std::string tasks_before;
    std::string actions_before;
    std::string budget_before;

    {
        gaudere::persistence::sqlite::ActionStore actions(database.path.string());
        gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
        gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
        gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
        for (int index = 1; index <= 3; ++index) {
            tasks.save(provider_history_task(index));
            actions.save(provider_history_action(index));
        }
        tasks.save(reflection_source("history-source-" + mode));
        expect(budgets.consume(scope, "historical-permit-1",
                               fake_now - 48min, policy)
                   == gaudere::budget::ConsumeResult::accepted
                   && budgets.consume(scope, "historical-permit-2",
                                      fake_now - 32min, policy)
                       == gaudere::budget::ConsumeResult::accepted
                   && budgets.consume(scope, "historical-permit-3",
                                      fake_now - 16min, policy)
                       == gaudere::budget::ConsumeResult::accepted,
               "W24 fixture contains exactly three admissible historical permits");
        const auto snapshot = budgets.snapshot(scope, fake_now, policy);
        expect(snapshot.total_used == 3 && snapshot.in_window_used == 3,
               "W24 production-like budget starts at three durable consumptions");

        tasks_before = table_snapshot(database.path, "tasks");
        actions_before = table_snapshot(database.path, "actions");
        budget_before = table_snapshot(database.path, "budget_consumptions");
        WakeRuntime wake_runtime(
            wakes, [&fake_now] { return fake_now; }, explicit_wake_scope,
            {explicit_wake_max_total});
        ExplicitWake explicit_wake(tasks, wake_runtime);
        expect(explicit_wake.accept("history-source-" + mode).result
                   == ExplicitWakeAcceptResult::accepted,
               "W24 explicit wake accepts without changing provider history");
    }

    {
        gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
        gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
        WakeRuntime wake_runtime(
            wakes, [&fake_now] { return fake_now; }, explicit_wake_scope,
            {explicit_wake_max_total});
        ExplicitWake explicit_wake(tasks, wake_runtime);
        const auto durable = explicit_wake.find("history-source-" + mode);
        expect(durable && durable->status == WakeIntentStatus::scheduled,
               "W24 restart recovers the sole scheduled wake beside provider history");
        if (durable && terminal == ProductionTerminal::fire) {
            fake_now = durable->due_at;
            const auto result = wake_runtime.reconcile();
            expect(result.fired == 1 && result.manual_review == 0,
                   "W24 restart fires the inert wake once at due");
        } else if (durable) {
            fake_now = durable->accepted_at + 1s;
            expect(explicit_wake.revoke("history-source-" + mode,
                                        "production-like revoke proof")
                       == gaudere::scheduling::wake::WakeIntentRevokeResult::revoked,
                   "W24 restart revokes the inert wake strictly before due");
        }
    }

    const auto tasks_after = table_snapshot(database.path, "tasks");
    const auto actions_after = table_snapshot(database.path, "actions");
    const auto budget_after = table_snapshot(database.path, "budget_consumptions");
    expect(tasks_after == tasks_before && actions_after == actions_before
               && budget_after == budget_before,
           "W24 Tasks/results/usage metadata, Actions, and budget rows are byte-equivalent");
    expect(count_rows(database.path, "tasks") == 4
               && count_rows(database.path, "actions") == 3
               && count_rows(database.path, "budget_consumptions") == 3
               && count_rows(database.path, "wake_intents") == 1,
           "W24 lifecycle creates only the one inert WakeIntent row");
    gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
    const auto final_budget = budgets.snapshot(scope, fake_now, policy);
    expect(final_budget.total_used == 3 && final_budget.in_window_used == 3,
           "W24 fire/revoke and restart consume no fourth provider permit");
}

void test_production_like_history_is_unchanged()
{
    run_production_like_history_fixture(ProductionTerminal::fire);
    run_production_like_history_fixture(ProductionTerminal::revoke);
}

void test_wake_event_does_not_create_unrelated_work()
{
    TemporaryDatabase database("w25-nonterminal-task");
    ControllerHarness harness(
        database.path, [] { return std::chrono::system_clock::now(); });
    gaudere::persistence::sqlite::ActionStore actions(database.path.string());
    gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
    const auto policy = openai_bootstrap_budget_policy();
    const std::string scope(openai_budget_scope());
    const auto budget_before = table_snapshot(database.path, "budget_consumptions");
    expect(!actions.has_running(),
           "W25 fixture begins without any active external Action");

    expect(harness.wake_runtime.accept("task-coincidence", "task-source", 300ms)
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "W25 inert wake is accepted before the quiescence violation fixture");
    expect(harness.controller.start(), "W25 controller starts");
    expect(harness.controller.wait_and_run() == WorkCycleResult::idle,
           "W25 initial event arms the future wake while work is quiescent");
    expect(harness.runtime.submit(
               task_definition("preexisting-task", "local.echo", "ordinary work"))
               == gaudere::work::SubmitResult::accepted,
           "W25 unrelated Task becomes pending without a new scheduler notification");
    expect(count_rows(database.path, "tasks") == 1,
           "W25 exactly one unrelated Task predates the wake event");

    expect(harness.controller.wait_and_run() == WorkCycleResult::worked,
           "W25 wake deadline event also runs already-authorized ordinary work");
    const auto wake = harness.wake_runtime.find("task-coincidence");
    const auto task = harness.tasks.find("preexisting-task");
    expect(wake && wake->status == WakeIntentStatus::fired
               && task && task->status == TaskStatus::succeeded
               && task->attempts_started == 1,
           "W25 durable states distinguish inert fire from normal dispatcher work");
    expect(count_rows(database.path, "tasks") == 1
               && count_rows(database.path, "actions") == 0
               && table_snapshot(database.path, "budget_consumptions")
                    == budget_before,
           "W25 wake creates no successor Task, Action, or provider consumption");
    const auto budget_after = budgets.snapshot(scope, rounded_now(), policy);
    expect(budget_after.total_used == 0,
           "W25 provider budget remains unused despite coincident normal work");
    expect(!harness.scheduler.next(),
           "W25 completed states leave no polling or successor deadline");
    harness.controller.stop();
    expect(harness.controller.wait_and_run() == WorkCycleResult::stopped,
           "W25 controller stops after the separation proof");
}

} // namespace

int main()
{
    test_uncommitted_acceptance_rolls_back_after_hard_exit();
    test_committed_acceptance_rearms_after_hard_exit();
    test_forward_clock_jump_reconciles_on_scheduler_event();
    test_queued_revoke_processed_at_due_fires();
    test_mailbox_commit_order_survives_stop_and_restart();
    test_busy_worker_fires_once_after_due();
    test_lease_earlier_than_wake_preserves_order();
    test_equal_lease_and_wake_deadlines_share_one_worker_event();
    test_production_like_history_is_unchanged();
    test_wake_event_does_not_create_unrelated_work();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All WakeIntent adversarial tests passed\n";
    return 0;
}
