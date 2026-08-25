#include "ExplicitWake.hpp"
#include "ResumeAfterWakeV1Prepare.hpp"
#include "WakeSourceDecision.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;
using namespace std::chrono_literals;
using WakeRuntime = gaudere::scheduling::wake::WakeIntentRuntime;

int failures = 0;

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
            / ("gaudere-agent-resume-v1-prepare-" + std::move(label) + "-"
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

std::int64_t count_rows(const std::filesystem::path& path, const char* table)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open sqlite test database");
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("cannot prepare row count");
    }
    const auto step = sqlite3_step(statement);
    const auto count = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    if (count < 0) throw std::runtime_error("cannot read row count");
    return count;
}

bool table_exists(const std::filesystem::path& path, const char* table)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot open sqlite test database");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
            -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        throw std::runtime_error("cannot prepare table lookup");
    }
    sqlite3_bind_text(statement, 1, table, -1, SQLITE_STATIC);
    const bool exists = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return exists;
}

std::string source_output()
{
    return Json{{"decision", "propose_wake"},
                {"reason", "Resume after a one-hour observation window to verify durable wake evidence."},
                {"schema", "gaudere.cognition.decision.v1"},
                {"wake_after_seconds", 3600}}.dump();
}

gaudere::work::Task source_task(const std::string& id)
{
    gaudere::work::Task task;
    task.id = id;
    task.idempotency_key = "cognition.reflect.v1:" + id;
    task.kind = "cognition.reflect.v1";
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "bounded historical reflection";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        "application/vnd.gaudere.cognition-decision+json",
        source_output(), {}, {}};
    return task;
}

std::string context_request(const std::string& suffix = {})
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/markdown; charset=utf-8"},
        {"content",
         "Current durable state: first wake PASS; runtime-downtime PASS; journal PASS; "
         "reliability condition already identified. Historical verification is DONE."
         + suffix},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", "resume-v1-prepare-provider-free-proof"},
            {"sha256", std::string(64, '0')}
        }})}
    }.dump();
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path,
                     gaudere::work::TimePoint initial =
                         gaudere::work::TimePoint{1000000ms})
        : tasks(path.string()), wakes(path.string()), now(initial),
          wake_runtime(wakes, [this] { return now; }, bounded_reflection_wake_scope,
                       {1}),
          explicit_wake(tasks, wake_runtime),
          work_runtime(tasks, [this] { return now; })
    {
        work_runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint now;
    WakeRuntime wake_runtime;
    ExplicitWake explicit_wake;
    gaudere::work::Runtime work_runtime;
};

std::string create_fired_wake(Fixture& fixture,
                              const std::string& id = "prepare-wake")
{
    fixture.tasks.save(source_task(id));
    const auto accepted = fixture.explicit_wake.accept(id);
    if (accepted.result != ExplicitWakeAcceptResult::accepted || !accepted.intent)
        throw std::runtime_error("could not accept wake fixture");
    fixture.now = accepted.intent->due_at;
    if (fixture.wake_runtime.reconcile().fired != 1)
        throw std::runtime_error("could not fire wake fixture");
    return id;
}

void expect_no_external_tables(const std::filesystem::path& path,
                               const std::string& label)
{
    expect(!table_exists(path, "actions"), label + ": no ActionStore table created");
    expect(!table_exists(path, "budget_consumptions"),
           label + ": no BudgetStore table created");
}

void test_prepare_and_duplicate()
{
    TemporaryDatabase database("duplicate");
    Fixture fixture(database.path);
    const auto wake_id = create_fired_wake(fixture);
    fixture.now += 1min;
    const auto request = context_request();
    std::vector<std::string> phases;
    ResumeAfterWakeV1Prepare prepare(
        fixture.tasks, fixture.wakes, fixture.work_runtime,
        [&fixture] { return fixture.now; }, true,
        [&phases](const std::string_view phase) { phases.emplace_back(phase); });

    const auto first = prepare.prepare(wake_id, request);
    expect(first.prepared && !first.duplicate, "first v1 preparation accepted");
    expect(first.selection_task && first.snapshot_task && first.claim.task,
           "first preparation exposes all durable identities");
    expect(first.claim.result == ResumeAfterWakeV1ClaimResult::accepted,
           "first preparation creates one resume v1 claim");
    expect(phases == std::vector<std::string>{
               "selection_durable", "snapshot_durable", "claim_durable"},
           "preparation reports three durable phase boundaries");
    const auto rows = count_rows(database.path, "tasks");
    expect(rows == 4, "source + selection + snapshot + resume are the only Tasks");

    const auto second = prepare.prepare(wake_id, request);
    expect(second.prepared && second.duplicate,
           "same request is idempotent after complete preparation");
    expect(second.selection_task && first.selection_task
               && second.selection_task->id == first.selection_task->id,
           "duplicate preserves selection identity");
    expect(second.snapshot_task && first.snapshot_task
               && second.snapshot_task->id == first.snapshot_task->id,
           "duplicate preserves exact snapshot identity");
    expect(second.claim.task && first.claim.task
               && second.claim.task->id == first.claim.task->id,
           "duplicate preserves resume identity");
    expect(count_rows(database.path, "tasks") == rows,
           "duplicate creates no extra Task");

    const auto conflict = prepare.prepare(wake_id, context_request(" changed"));
    expect(!conflict.prepared
               && conflict.detail.find("different request bytes") != std::string::npos,
           "different request after first selection fails closed");
    expect(count_rows(database.path, "tasks") == rows,
           "conflicting request creates no new snapshot");
    expect_no_external_tables(database.path, "duplicate proof");
}

void test_crash_after_selection_reuses_capture()
{
    TemporaryDatabase database("crash-selection");
    std::string wake_id;
    std::string request;
    gaudere::work::TimePoint retry_now;
    {
        Fixture fixture(database.path);
        wake_id = create_fired_wake(fixture, "crash-selection-wake");
        fixture.now += 1min;
        request = context_request();
        ResumeAfterWakeV1Prepare prepare(
            fixture.tasks, fixture.wakes, fixture.work_runtime,
            [&fixture] { return fixture.now; }, true,
            [](const std::string_view phase) {
                if (phase == "selection_durable")
                    throw std::runtime_error("synthetic crash after selection");
            });
        bool crashed = false;
        try { (void)prepare.prepare(wake_id, request); }
        catch (const std::runtime_error&) { crashed = true; }
        expect(crashed, "synthetic crash occurs after durable selection");
        expect(count_rows(database.path, "tasks") == 2,
               "crash after selection leaves source + one selection only");
        retry_now = fixture.now + 5min;
    }

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::work::Runtime runtime(tasks, [&] { return retry_now; });
    runtime.recover();
    ResumeAfterWakeV1Prepare prepare(
        tasks, wakes, runtime, [&] { return retry_now; }, true);
    const auto retry = prepare.prepare(wake_id, request);
    expect(retry.prepared && retry.duplicate,
           "retry after selection crash completes original preparation");
    expect(retry.snapshot_task && retry.claim.task,
           "retry creates snapshot and claim from frozen selection");
    expect(count_rows(database.path, "tasks") == 4,
           "selection crash retry ends with exactly four Tasks");
    expect_no_external_tables(database.path, "selection crash proof");
}

void test_crash_after_snapshot_reuses_exact_snapshot()
{
    TemporaryDatabase database("crash-snapshot");
    std::string wake_id;
    std::string request;
    gaudere::work::TimePoint retry_now;
    {
        Fixture fixture(database.path);
        wake_id = create_fired_wake(fixture, "crash-snapshot-wake");
        fixture.now += 1min;
        request = context_request();
        ResumeAfterWakeV1Prepare prepare(
            fixture.tasks, fixture.wakes, fixture.work_runtime,
            [&fixture] { return fixture.now; }, true,
            [](const std::string_view phase) {
                if (phase == "snapshot_durable")
                    throw std::runtime_error("synthetic crash after snapshot");
            });
        bool crashed = false;
        try { (void)prepare.prepare(wake_id, request); }
        catch (const std::runtime_error&) { crashed = true; }
        expect(crashed, "synthetic crash occurs after durable snapshot");
        expect(count_rows(database.path, "tasks") == 3,
               "crash after snapshot leaves exactly source + selection + snapshot");
        retry_now = fixture.now + 5min;
    }

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::work::Runtime runtime(tasks, [&] { return retry_now; });
    runtime.recover();
    ResumeAfterWakeV1Prepare prepare(
        tasks, wakes, runtime, [&] { return retry_now; }, true);
    const auto retry = prepare.prepare(wake_id, request);
    expect(retry.prepared && retry.duplicate,
           "retry after snapshot crash reuses durable snapshot");
    expect(count_rows(database.path, "tasks") == 4,
           "snapshot crash retry adds only the missing resume Task");
    expect_no_external_tables(database.path, "snapshot crash proof");
}

void test_retry_after_freshness_window_does_not_forge_new_capture()
{
    TemporaryDatabase database("stale-retry");
    std::string wake_id;
    std::string request;
    gaudere::work::TimePoint retry_now;
    {
        Fixture fixture(database.path);
        wake_id = create_fired_wake(fixture, "stale-retry-wake");
        fixture.now += 1min;
        request = context_request();
        ResumeAfterWakeV1Prepare prepare(
            fixture.tasks, fixture.wakes, fixture.work_runtime,
            [&fixture] { return fixture.now; }, true,
            [](const std::string_view phase) {
                if (phase == "selection_durable")
                    throw std::runtime_error("synthetic crash before snapshot");
            });
        try { (void)prepare.prepare(wake_id, request); }
        catch (const std::runtime_error&) {}
        retry_now = fixture.now + 16min;
    }

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::work::Runtime runtime(tasks, [&] { return retry_now; });
    runtime.recover();
    ResumeAfterWakeV1Prepare prepare(
        tasks, wakes, runtime, [&] { return retry_now; }, true);
    const auto retry = prepare.prepare(wake_id, request);
    expect(!retry.prepared
               && retry.claim.result == ResumeAfterWakeV1ClaimResult::stale,
           "late retry is stale instead of receiving a forged fresh capture time");
    expect(count_rows(database.path, "tasks") == 3,
           "late retry records frozen snapshot but no resume Task");
    expect_no_external_tables(database.path, "stale retry proof");
}

void test_disabled_has_no_effect()
{
    TemporaryDatabase database("disabled");
    Fixture fixture(database.path);
    const auto wake_id = create_fired_wake(fixture, "disabled-wake");
    const auto before = count_rows(database.path, "tasks");
    ResumeAfterWakeV1Prepare disabled(
        fixture.tasks, fixture.wakes, fixture.work_runtime,
        [&fixture] { return fixture.now; }, false);
    const auto result = disabled.prepare(wake_id, context_request());
    expect(!result.prepared && result.detail.find("disabled") != std::string::npos,
           "v1 preparation remains disabled by default");
    expect(count_rows(database.path, "tasks") == before,
           "disabled preparation creates no Task");
    expect_no_external_tables(database.path, "disabled proof");
}

} // namespace

int main()
{
    try {
        test_prepare_and_duplicate();
        test_crash_after_selection_reuses_capture();
        test_crash_after_snapshot_reuses_exact_snapshot();
        test_retry_after_freshness_window_does_not_forge_new_capture();
        test_disabled_has_no_effect();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " resume-v1-prepare assertion(s) failed\n";
        return 1;
    }
    std::cout << "resume-after-wake v1 prepare provider-free tests: PASS\n";
    return 0;
}
