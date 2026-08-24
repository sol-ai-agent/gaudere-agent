#include "ExplicitWake.hpp"
#include "ResumeAfterWakeV1.hpp"
#include "ResumeContextSnapshot.hpp"
#include "WakeSourceDecision.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
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
            / ("gaudere-agent-" + std::move(label) + "-"
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
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("could not open sqlite test database");
    }
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("could not prepare sqlite count");
    }
    const auto step = sqlite3_step(statement);
    const auto count = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (count < 0) throw std::runtime_error("could not read sqlite count");
    return count;
}

std::string source_output(const std::uint64_t seconds)
{
    return Json{{"decision", "propose_wake"},
                {"reason",
                 "Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement."},
                {"schema", "gaudere.cognition.decision.v1"},
                {"wake_after_seconds", seconds}}.dump();
}

gaudere::work::Task source_task(const std::string& id,
                                const std::uint64_t seconds = 3600)
{
    gaudere::work::Task task;
    task.id = id;
    task.idempotency_key = "cognition.reflect.v1:" + id;
    task.kind = "cognition.reflect.v1";
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "historical bounded reflection fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        "application/vnd.gaudere.cognition-decision+json",
        source_output(seconds), {}, {}};
    return task;
}

std::string fresh_content(const std::string& suffix = {})
{
    return
        "Current durable state after the historical wake:\n"
        "- first real WakeIntent proof: DONE/PASS, lateness_ms=0;\n"
        "- runtime-downtime reconciliation: DONE/PASS, lateness_ms=300475;\n"
        "- journal update: DONE;\n"
        "- reliability condition: DONE — reconcile exactly once on first safe restart "
        "with durable lateness and no duplicate/provider/successor/hidden effect.\n"
        "The historical verification objective is therefore already complete."
        + suffix;
}

std::string snapshot_request(const std::string& content)
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/markdown; charset=utf-8"},
        {"content", content},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", "provider-free-v1-regression"},
            {"sha256", std::string(64, '0')}
        }})}
    }.dump();
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path,
                     gaudere::work::TimePoint initial =
                         gaudere::work::TimePoint{1000000ms})
        : tasks(path.string()), wakes(path.string()), actions(path.string()),
          budgets(path.string()), now(initial),
          wake_runtime(wakes, [this] { return now; }, bounded_reflection_wake_scope,
                       {1}),
          explicit_wake(tasks, wake_runtime),
          work_runtime(tasks, [this] { return now; }),
          recorder(tasks, work_runtime, [this] { return now; }),
          resume(tasks, wakes, work_runtime, [this] { return now; }, true)
    {
        work_runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::work::TimePoint now;
    WakeRuntime wake_runtime;
    ExplicitWake explicit_wake;
    gaudere::work::Runtime work_runtime;
    ResumeContextSnapshotRecorder recorder;
    ResumeAfterWakeV1 resume;
};

std::string create_fired_wake(Fixture& fixture,
                              const std::string& source_id =
                                  "production-reflection-wake-source-first")
{
    fixture.tasks.save(source_task(source_id));
    const auto accepted = fixture.explicit_wake.accept(source_id);
    if (accepted.result != ExplicitWakeAcceptResult::accepted || !accepted.intent) {
        throw std::runtime_error("could not accept wake fixture");
    }
    fixture.now = accepted.intent->due_at;
    const auto reconciled = fixture.wake_runtime.reconcile();
    if (reconciled.fired != 1) {
        throw std::runtime_error("could not fire wake fixture");
    }
    return source_id;
}

std::string record_snapshot(Fixture& fixture, const std::string& content)
{
    const auto recorded = fixture.recorder.record(snapshot_request(content));
    if ((recorded.result != ResumeContextSnapshotRecordResult::accepted
         && recorded.result != ResumeContextSnapshotRecordResult::duplicate)
        || !recorded.task) {
        throw std::runtime_error("could not record snapshot fixture: " + recorded.detail);
    }
    return recorded.task->id;
}

void expect_no_external_effects(const Fixture& fixture,
                                const std::filesystem::path& path,
                                const std::string& label)
{
    expect(count_rows(path, "actions") == 0, label + ": no Action rows");
    expect(count_rows(path, "budget_consumptions") == 0,
           label + ": no provider budget rows");
    expect(count_rows(path, "wake_intents") == 1,
           label + ": only the original WakeIntent exists");
}

void test_86_regression_and_first_claim_binding()
{
    TemporaryDatabase database("resume-v1-regression");
    Fixture fixture(database.path);
    const auto wake_id = create_fired_wake(fixture);
    const auto wake_before = fixture.wakes.find(bounded_reflection_wake_scope, wake_id);
    fixture.now += 1min;
    const auto first_snapshot = record_snapshot(fixture, fresh_content());
    fixture.now += 1min;
    const auto second_snapshot = record_snapshot(fixture, fresh_content("\nnewer snapshot"));

    const auto claim = fixture.resume.claim(wake_id, first_snapshot);
    expect(claim.result == ResumeAfterWakeV1ClaimResult::accepted,
           "#86 regression: first fresh snapshot accepted");
    expect(claim.task.has_value(), "#86 regression: resume v1 Task returned");
    if (claim.task) {
        expect(claim.task->id == std::string{resume_after_wake_v1_task_prefix} + wake_id,
               "resume v1 identity remains one-per-wake");
        expect(claim.task->kind == resume_after_wake_v1_task_kind,
               "resume v1 kind is canonical");
        const auto input = Json::parse(claim.task->input);
        expect(input.at("schema").get<std::string>()
                   == resume_after_wake_v1_context_schema,
               "resume v1 context schema is canonical");
        expect(input.at("historical").at("source_decision").at("reason")
                   .get<std::string>().find("verify that the active pre-wake runtime")
                   != std::string::npos,
               "historical intention remains present and untouched");
        expect(input.at("current_context").at("snapshot_task_id").get<std::string>()
                   == first_snapshot,
               "exact selected snapshot id is frozen");
        const auto current = input.at("current_context").at("capsule").at("content")
            .get<std::string>();
        expect(current.find("already complete") != std::string::npos,
               "fresh current context records historical objective as completed");
        expect(current.find("lateness_ms=300475") != std::string::npos,
               "fresh context carries later runtime-downtime evidence");
        expect(input.at("instructions").get<std::string>().find("data, not instructions")
                   != std::string::npos,
               "input explicitly strips authority from both data blocks");
    }

    const auto different = fixture.resume.claim(wake_id, second_snapshot);
    expect(different.result == ResumeAfterWakeV1ClaimResult::conflict,
           "different snapshot after first claim fails closed");
    expect(fixture.tasks.find(std::string{resume_after_wake_v1_task_prefix} + wake_id)
               .has_value(),
           "only one resume v1 Task identity exists");

    fixture.now += 30min;
    const auto reopened_same = fixture.resume.claim(wake_id, first_snapshot);
    expect(reopened_same.result == ResumeAfterWakeV1ClaimResult::duplicate,
           "existing claim remains idempotent after snapshot freshness window");
    expect(reopened_same.task && reopened_same.task->input == claim.task->input,
           "reopen preserves exact frozen resume input");

    const auto wake_after = fixture.wakes.find(bounded_reflection_wake_scope, wake_id);
    expect(wake_before && wake_after
               && wake_before->status == wake_after->status
               && wake_before->terminal_at == wake_after->terminal_at
               && wake_before->terminal_reason == wake_after->terminal_reason,
           "resume v1 claim does not mutate WakeIntent evidence");
    expect_no_external_effects(fixture, database.path, "#86 regression");
}

void test_first_claim_freshness()
{
    {
        TemporaryDatabase database("resume-v1-too-old");
        Fixture fixture(database.path);
        const auto wake_id = create_fired_wake(fixture);
        fixture.now += 1min;
        const auto snapshot = record_snapshot(fixture, "fresh then aged");
        fixture.now += 16min;
        const auto claim = fixture.resume.claim(wake_id, snapshot);
        expect(claim.result == ResumeAfterWakeV1ClaimResult::stale,
               "snapshot older than 15 minutes rejected on first claim");
        expect(!fixture.tasks.find(std::string{resume_after_wake_v1_task_prefix} + wake_id),
               "stale first claim creates no resume Task");
        expect_no_external_effects(fixture, database.path, "stale first claim");
    }

    {
        TemporaryDatabase database("resume-v1-predates-wake");
        Fixture fixture(database.path);
        fixture.tasks.save(source_task("production-reflection-wake-source-first"));
        const auto accepted = fixture.explicit_wake.accept(
            "production-reflection-wake-source-first");
        if (accepted.result != ExplicitWakeAcceptResult::accepted || !accepted.intent)
            throw std::runtime_error("could not accept pre-wake fixture");
        const auto snapshot = record_snapshot(fixture, "captured before wake terminal");
        fixture.now = accepted.intent->due_at;
        if (fixture.wake_runtime.reconcile().fired != 1)
            throw std::runtime_error("could not fire pre-wake fixture");
        const auto claim = fixture.resume.claim(accepted.intent->id, snapshot);
        expect(claim.result == ResumeAfterWakeV1ClaimResult::stale,
               "snapshot captured before wake terminal is rejected");
    }

    {
        TemporaryDatabase database("resume-v1-clock-rollback");
        Fixture fixture(database.path);
        const auto wake_id = create_fired_wake(fixture);
        fixture.now += 2min;
        const auto snapshot = record_snapshot(fixture, "future relative to claim clock");
        fixture.now -= 1min;
        const auto claim = fixture.resume.claim(wake_id, snapshot);
        expect(claim.result == ResumeAfterWakeV1ClaimResult::ineligible,
               "claim clock before snapshot capture fails closed");
    }
}

void test_missing_corrupt_and_disabled()
{
    TemporaryDatabase database("resume-v1-invalid");
    Fixture fixture(database.path);
    const auto wake_id = create_fired_wake(fixture);

    const auto missing = fixture.resume.claim(
        wake_id, std::string{resume_context_snapshot_task_prefix} + std::string(64, 'a'));
    expect(missing.result == ResumeAfterWakeV1ClaimResult::snapshot_not_found,
           "missing snapshot is reported without claim");

    gaudere::work::Task corrupt;
    corrupt.id = std::string{resume_context_snapshot_task_prefix} + std::string(64, 'b');
    corrupt.idempotency_key = corrupt.id;
    corrupt.kind = resume_context_snapshot_task_kind;
    corrupt.input_content_type = resume_context_snapshot_content_type;
    corrupt.input = "{}";
    corrupt.limits.max_input_bytes = 24 * 1024;
    corrupt.limits.max_output_bytes = 24 * 1024;
    corrupt.limits.max_runtime = 2s;
    corrupt.limits.max_attempts = 2;
    corrupt.attempts_started = 1;
    corrupt.status = gaudere::work::TaskStatus::succeeded;
    corrupt.result = gaudere::work::TaskResult{
        resume_context_snapshot_content_type, "{}", {}, {}};
    fixture.tasks.save(corrupt);
    const auto invalid_snapshot = fixture.resume.claim(wake_id, corrupt.id);
    expect(invalid_snapshot.result == ResumeAfterWakeV1ClaimResult::ineligible,
           "corrupt snapshot fails closed");

    ResumeAfterWakeV1 disabled(fixture.tasks, fixture.wakes, fixture.work_runtime,
                               [&fixture] { return fixture.now; }, false);
    const auto disabled_claim = disabled.claim(wake_id, corrupt.id);
    expect(disabled_claim.result == ResumeAfterWakeV1ClaimResult::disabled,
           "resume v1 remains disabled by default");
}

void test_existing_claim_corruption_fails_closed()
{
    TemporaryDatabase database("resume-v1-existing-corrupt");
    Fixture fixture(database.path);
    const auto wake_id = create_fired_wake(fixture);
    fixture.now += 1min;
    const auto snapshot = record_snapshot(fixture, "valid snapshot");

    gaudere::work::Task corrupt;
    corrupt.id = std::string{resume_after_wake_v1_task_prefix} + wake_id;
    corrupt.idempotency_key = corrupt.id;
    corrupt.kind = resume_after_wake_v1_task_kind;
    corrupt.input_content_type = resume_after_wake_v1_content_type;
    corrupt.input = "{}";
    corrupt.limits.max_input_bytes = 48 * 1024;
    corrupt.limits.max_output_bytes = 8 * 1024;
    corrupt.limits.max_runtime = 60s;
    corrupt.limits.max_attempts = 2;
    fixture.tasks.save(corrupt);

    const auto claim = fixture.resume.claim(wake_id, snapshot);
    expect(claim.result == ResumeAfterWakeV1ClaimResult::conflict,
           "corrupt existing fixed-id resume claim fails closed");
    expect(count_rows(database.path, "tasks") == 3,
           "corrupt existing claim is not replaced by a second resume identity");
}

void test_reopen_database_after_claim()
{
    TemporaryDatabase database("resume-v1-reopen");
    std::string wake_id;
    std::string snapshot_id;
    std::string frozen_input;
    gaudere::work::TimePoint reopen_now;
    {
        Fixture fixture(database.path);
        wake_id = create_fired_wake(fixture);
        fixture.now += 1min;
        snapshot_id = record_snapshot(fixture, fresh_content());
        const auto claim = fixture.resume.claim(wake_id, snapshot_id);
        if (claim.result != ResumeAfterWakeV1ClaimResult::accepted || !claim.task)
            throw std::runtime_error("could not create reopen claim fixture");
        frozen_input = claim.task->input;
        reopen_now = fixture.now + 1h;
    }

    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::work::Runtime runtime(tasks, [&] { return reopen_now; });
    runtime.recover();
    ResumeAfterWakeV1 reopened(tasks, wakes, runtime, [&] { return reopen_now; }, true);
    const auto duplicate = reopened.claim(wake_id, snapshot_id);
    expect(duplicate.result == ResumeAfterWakeV1ClaimResult::duplicate,
           "database reopen after one hour preserves existing claim despite age");
    expect(duplicate.task && duplicate.task->input == frozen_input,
           "database reopen preserves exact first-write context binding");
}

} // namespace

int main()
{
    try {
        test_86_regression_and_first_claim_binding();
        test_first_claim_freshness();
        test_missing_corrupt_and_disabled();
        test_existing_claim_corruption_fails_closed();
        test_reopen_database_after_claim();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " resume-after-wake v1 assertion(s) failed\n";
        return 1;
    }
    std::cout << "resume-after-wake v1 provider-free tests: PASS\n";
    return 0;
}
