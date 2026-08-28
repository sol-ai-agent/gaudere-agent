#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "CurrentCognitionCycle.hpp"
#include "OpenAIBudget.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;
using namespace std::chrono_literals;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabases {
    explicit TemporaryDatabases(std::string label)
    {
        const auto token = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto root = std::filesystem::temp_directory_path();
        generic = root / ("gaudere-pulse-generic-" + label + "-" + token + ".db");
        sidecar = root / ("gaudere-pulse-sidecar-" + label + "-" + token + ".db");
    }

    ~TemporaryDatabases()
    {
        remove_database(generic);
        remove_database(sidecar);
    }

    static void remove_database(const std::filesystem::path& path)
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path generic;
    std::filesystem::path sidecar;
};

std::string query_text(const std::filesystem::path& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) throw std::runtime_error("could not open sqlite database");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("could not prepare sqlite query");
    }
    const auto step = sqlite3_step(statement);
    std::string result;
    if (step == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        const auto* value = sqlite3_column_text(statement, 0);
        const auto bytes = sqlite3_column_bytes(statement, 0);
        result.assign(reinterpret_cast<const char*>(value),
                      static_cast<std::size_t>(bytes));
    } else if (step != SQLITE_ROW && step != SQLITE_DONE) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("could not execute sqlite query");
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::int64_t query_int(const std::filesystem::path& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) throw std::runtime_error("could not open sqlite database");
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("could not prepare sqlite integer query");
    }
    const auto step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("could not read sqlite integer");
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

void execute_sql(const std::filesystem::path& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr)
        != SQLITE_OK) throw std::runtime_error("could not open writable sqlite database");
    char* error = nullptr;
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);
}

std::string table_set(const std::filesystem::path& path)
{
    return query_text(path,
        "SELECT group_concat(name, ',') FROM ("
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name)");
}

std::int64_t row_count(const std::filesystem::path& path, const char* table)
{
    return query_int(path, "SELECT COUNT(*) FROM " + std::string(table));
}

std::int64_t user_version(const std::filesystem::path& path)
{
    return query_int(path, "PRAGMA user_version");
}

std::string decision_continue(const std::string& objective)
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "continue"},
                {"reason", "Current durable evidence supports another bounded step."},
                {"objective", objective}}.dump();
}

std::string decision_stop()
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "stop"},
                {"reason", "No useful bounded objective remains in current evidence."}}
        .dump();
}

std::string snapshot_request(const std::string& content,
                             const std::string& ref)
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/plain; charset=utf-8"},
        {"content", content},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", ref},
            {"sha256", sha256_hex(content)}
        }})}
    }.dump();
}

Task bootstrap_resume_task()
{
    Task task;
    task.id = "cognition.resume-after-wake.v0:autonomous-pulse-bootstrap";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "provider-free pulse bootstrap fixture";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("Create one canonical current cognition seed."), {}, {}};
    return task;
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path,
                     const std::int64_t start_ms = 1000000000)
        : tasks(path.string()), actions(path.string()), budgets(path.string()),
          wakes(path.string()), now(gaudere::work::TimePoint{
              std::chrono::milliseconds{start_ms}}),
          runtime(tasks, [this] { return now; })
    {
        runtime.recover();
    }

    std::string make_seed_current()
    {
        const auto bootstrap = bootstrap_resume_task();
        tasks.save(bootstrap);
        ResumeContextSnapshotRecorder recorder(
            tasks, runtime, [this] { return now; });
        const auto snapshot = recorder.record(snapshot_request(
            "A canonical current cognition seed is required for the autonomous pulse proof.",
            "autonomous-pulse-seed"));
        if ((snapshot.result != ResumeContextSnapshotRecordResult::accepted
             && snapshot.result != ResumeContextSnapshotRecordResult::duplicate)
            || !snapshot.task) {
            throw std::runtime_error("could not create seed snapshot");
        }
        CurrentCognitionCycle cycle(tasks, runtime, [this] { return now; }, true);
        const auto claim = cycle.claim(bootstrap.id, snapshot.task->id);
        if ((claim.result != CurrentCognitionClaimResult::accepted
             && claim.result != CurrentCognitionClaimResult::duplicate)
            || !claim.task) {
            throw std::runtime_error("could not create seed current cognition");
        }
        auto completed = *claim.task;
        completed.attempts_started = 1;
        completed.status = TaskStatus::succeeded;
        completed.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type,
            decision_continue("Prepare the next bounded autonomy step from current facts."),
            {}, {}};
        tasks.save(completed);
        if (!valid_current_cognition_task(completed))
            throw std::runtime_error("seed current cognition is non-canonical");
        return completed.id;
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint now;
    gaudere::work::Runtime runtime;
};

void finish_current(Fixture& fixture,
                    const std::string& id,
                    const std::string& output,
                    const TaskStatus status = TaskStatus::succeeded)
{
    auto task = fixture.tasks.find(id);
    if (!task) throw std::runtime_error("current cognition Task missing in fixture");
    task->attempts_started = 1;
    task->status = status;
    if (status == TaskStatus::succeeded) {
        task->result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type, output, {}, {}};
    } else {
        task->result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type, "ambiguous", "effect_unknown",
            "synthetic ambiguous provider boundary"};
    }
    fixture.tasks.save(*task);
}

void test_sha_and_sidecar_isolation()
{
    expect(sha256_hex("abc")
        == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "internal SHA-256 matches known vector");

    TemporaryDatabases database("isolation");
    Fixture fixture(database.generic);
    expect(user_version(database.generic) == 4, "generic Gaudere DB remains schema v4");
    const auto tables_before = table_set(database.generic);
    expect(tables_before == "actions,budget_consumptions,tasks,wake_intents",
           "generic Gaudere table set is exact before sidecar");

    {
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        expect(user_version(database.sidecar) == 1, "pulse sidecar uses schema v1");
        expect(table_set(database.sidecar) == "autonomous_cognition_pulse_cursor",
               "sidecar contains only pulse cursor table");
        AutonomousCognitionPulse disabled(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, false);
        expect(disabled.seed("nonexistent").result == PulseResult::disabled,
               "disabled seed is inert");
        expect(disabled.observe().result == PulseResult::disabled,
               "disabled observation is inert");
        expect(!sidecar.find(autonomous_cognition_pulse_scope),
               "disabled pulse writes no cursor row");
    }

    expect(user_version(database.generic) == 4,
           "sidecar initialization does not change Core schema version");
    expect(table_set(database.generic) == tables_before,
           "sidecar initialization does not change Core table set");
}

void test_due_prepare_idempotence_and_continue_settlement()
{
    TemporaryDatabases database("normal");
    Fixture fixture(database.generic);
    const auto predecessor_id = fixture.make_seed_current();
    AutonomousCognitionPulseStore sidecar(database.sidecar.string());
    AutonomousCognitionPulse pulse(
        sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
        [&fixture] { return fixture.now; }, true);

    const auto seeded = pulse.seed(predecessor_id);
    expect(seeded.result == PulseResult::seeded && seeded.cursor,
           "pulse seeds from canonical current cognition");
    if (!seeded.cursor) return;
    const auto seed_cursor = *seeded.cursor;
    expect(seed_cursor.state == AutonomousCognitionPulseState::idle,
           "continue predecessor seeds idle cadence");
    expect(seed_cursor.due_at_ms - seed_cursor.anchor_at_ms
               == std::chrono::duration_cast<std::chrono::milliseconds>(6h).count(),
           "continue seed cadence is six hours");

    const auto tasks_before = row_count(database.generic, "tasks");
    const auto actions_before = row_count(database.generic, "actions");
    const auto budgets_before = row_count(database.generic, "budget_consumptions");
    const auto wakes_before = row_count(database.generic, "wake_intents");
    expect(pulse.observe().result == PulseResult::not_due,
           "pulse is inert before durable due time");

    fixture.now = gaudere::work::TimePoint{
        std::chrono::milliseconds{seed_cursor.due_at_ms}};
    const auto prepared = pulse.observe();
    expect(prepared.result == PulseResult::prepared && prepared.cursor && prepared.task,
           "due pulse prepares one current cognition Task");
    if (!prepared.cursor || !prepared.task) return;
    const auto prepared_cursor = *prepared.cursor;
    expect(prepared_cursor.state == AutonomousCognitionPulseState::prepared,
           "cursor becomes prepared");
    expect(prepared_cursor.generation == 0,
           "preparation does not advance generation before cognition settles");
    expect(prepared_cursor.observed_at_ms
               && *prepared_cursor.observed_at_ms == seed_cursor.due_at_ms,
           "first due observation is frozen exactly");
    expect(prepared.task->id == prepared_cursor.current_task_id
               && prepared.task->kind == current_cognition_task_kind,
           "prepared Task reuses existing current cognition identity");
    expect(prepared.task->status == TaskStatus::pending,
           "provider-free pulse leaves current cognition pending");
    expect(row_count(database.generic, "tasks") == tasks_before + 2,
           "one pulse adds exactly snapshot plus current cognition Task");
    expect(row_count(database.generic, "actions") == actions_before,
           "pulse creates no provider Action");
    expect(row_count(database.generic, "budget_consumptions") == budgets_before,
           "pulse consumes no provider budget");
    expect(row_count(database.generic, "wake_intents") == wakes_before,
           "pulse creates no WakeIntent");

    const auto snapshot = fixture.tasks.find(prepared_cursor.snapshot_task_id);
    expect(snapshot && snapshot->status == TaskStatus::succeeded && snapshot->result,
           "autonomous context snapshot is durable and succeeded");
    if (snapshot && snapshot->result) {
        const auto capsule = Json::parse(snapshot->result->output);
        const auto content = capsule.at("content").get<std::string>();
        const auto facts = Json::parse(content);
        expect(facts.at("schema") == "gaudere.autonomous-pulse-context.v0",
               "local context has autonomous pulse schema");
        expect(facts.at("authority").get<std::string>().find("no provider")
                   != std::string::npos,
               "local context explicitly grants no provider authority");
        expect(facts.at("pulse").at("lateness_ms") == 0,
               "on-time pulse records zero lateness");
        expect(facts.at("predecessor").at("task_id") == predecessor_id,
               "local context names exact predecessor");
        expect(facts.at("provider_budget").at("total_used") == 0,
               "local context records read-only provider budget");
        expect(facts.at("historical_wake").at("cardinality") == "empty",
               "local context records bounded historical wake summary");
        expect(content.find("GitHub") == std::string::npos
                   && content.find("Drive") == std::string::npos
                   && content.find("B10") == std::string::npos,
               "local context contains no connector/operator facts");
        const auto provenance = capsule.at("provenance").at(0);
        expect(provenance.at("sha256") == sha256_hex(content),
               "runtime provenance hashes exact local context bytes");
    }

    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto repeated = pulse.observe();
        expect(repeated.result == PulseResult::waiting
                   && repeated.cursor
                   && repeated.cursor->current_task_id
                       == prepared_cursor.current_task_id,
               "100 observations do not duplicate a prepared pulse");
    }
    expect(row_count(database.generic, "tasks") == tasks_before + 2,
           "repeated observations add no extra Tasks");

    finish_current(fixture, prepared_cursor.current_task_id,
                   decision_continue("Prove the next scheduled cognition opportunity."));
    fixture.now += 1min;
    const auto settled = pulse.observe();
    expect(settled.result == PulseResult::settled_continue && settled.cursor,
           "canonical continue cognition settles pulse");
    if (settled.cursor) {
        expect(settled.cursor->generation == 1,
               "successful cognition advances exactly one generation");
        expect(settled.cursor->predecessor_task_id
                   == prepared_cursor.current_task_id,
               "successful cognition becomes exact next predecessor");
        expect(settled.cursor->state == AutonomousCognitionPulseState::idle,
               "continue settlement returns to idle");
        expect(settled.cursor->due_at_ms - settled.cursor->anchor_at_ms
                   == std::chrono::duration_cast<std::chrono::milliseconds>(6h).count(),
               "continue settlement rearms six-hour cadence");
        expect(!settled.cursor->observed_at_ms
                   && settled.cursor->snapshot_task_id.empty()
                   && settled.cursor->current_task_id.empty(),
               "settlement clears previous generation work ids");
    }
    expect(row_count(database.generic, "actions") == actions_before,
           "settlement still creates no Action");
    expect(row_count(database.generic, "budget_consumptions") == budgets_before,
           "settlement still consumes no budget");
}

void test_stop_quiescence_and_terminal_blocking()
{
    {
        TemporaryDatabases database("stop");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("stop fixture seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}};
        const auto prepared = pulse.observe();
        if (!prepared.cursor) throw std::runtime_error("stop fixture prepare failed");
        finish_current(fixture, prepared.cursor->current_task_id, decision_stop());
        fixture.now += 1min;
        const auto settled = pulse.observe();
        expect(settled.result == PulseResult::settled_stop && settled.cursor,
               "canonical stop cognition settles pulse");
        if (settled.cursor) {
            expect(settled.cursor->state == AutonomousCognitionPulseState::quiescent,
                   "stop settlement enters quiescent state");
            expect(settled.cursor->due_at_ms - settled.cursor->anchor_at_ms
                       == std::chrono::duration_cast<std::chrono::milliseconds>(24h).count(),
                   "stop settlement uses 24-hour recheck cadence");
        }
    }

    {
        TemporaryDatabases database("manual-review");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("manual-review seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}};
        const auto prepared = pulse.observe();
        if (!prepared.cursor) throw std::runtime_error("manual-review prepare failed");
        finish_current(fixture, prepared.cursor->current_task_id,
                       "ignored", TaskStatus::manual_review);
        fixture.now += 1min;
        const auto blocked = pulse.observe();
        expect(blocked.result == PulseResult::blocked && blocked.cursor
                   && blocked.cursor->state == AutonomousCognitionPulseState::blocked,
               "manual-review cognition blocks cursor without replay");
        const auto tasks_after = row_count(database.generic, "tasks");
        expect(pulse.observe().result == PulseResult::blocked,
               "blocked pulse remains blocked on later observation");
        expect(row_count(database.generic, "tasks") == tasks_after,
               "blocked pulse creates no successor Task");
    }

    {
        TemporaryDatabases database("noncanonical");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("noncanonical seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}};
        const auto prepared = pulse.observe();
        if (!prepared.cursor) throw std::runtime_error("noncanonical prepare failed");
        finish_current(fixture, prepared.cursor->current_task_id,
                       "{\"decision\":\"continue\"}");
        fixture.now += 1min;
        expect(pulse.observe().result == PulseResult::blocked,
               "noncanonical succeeded cognition blocks cursor");
    }
}

void test_budget_time_and_catchup_guards()
{
    {
        TemporaryDatabases database("budget");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("budget seed failed");
        const auto due = gaudere::budget::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}};
        const auto policy = openai_bootstrap_budget_policy();
        for (int index = 4; index >= 1; --index) {
            const auto result = fixture.budgets.consume(
                std::string{openai_budget_scope()},
                "synthetic-budget-" + std::to_string(index),
                due - std::chrono::hours{index}, policy);
            expect(result == gaudere::budget::ConsumeResult::accepted,
                   "fixture budget consumption accepted");
        }
        const auto tasks_before = row_count(database.generic, "tasks");
        const auto budget_rows = row_count(database.generic, "budget_consumptions");
        fixture.now = due;
        const auto observation = pulse.observe();
        expect(observation.result == PulseResult::budget_blocked,
               "window-exhausted budget prevents pulse preparation");
        expect(row_count(database.generic, "tasks") == tasks_before,
               "budget-blocked pulse creates no Task");
        expect(row_count(database.generic, "budget_consumptions") == budget_rows,
               "budget eligibility check is read-only");
        const auto cursor = sidecar.find(autonomous_cognition_pulse_scope);
        expect(cursor && cursor->state == AutonomousCognitionPulseState::idle
                   && !cursor->observed_at_ms,
               "budget-blocked pulse does not freeze a generation");
    }

    {
        TemporaryDatabases database("rollback");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("rollback seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->anchor_at_ms - 1}};
        const auto before = sidecar.find(autonomous_cognition_pulse_scope);
        expect(pulse.observe().result == PulseResult::clock_rollback,
               "clock rollback fails closed");
        const auto after = sidecar.find(autonomous_cognition_pulse_scope);
        expect(before && after && before->revision == after->revision,
               "clock rollback does not move durable cursor");
    }

    {
        TemporaryDatabases database("catchup");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("catchup seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}} + 30h;
        const auto tasks_before = row_count(database.generic, "tasks");
        const auto prepared = pulse.observe();
        expect(prepared.result == PulseResult::prepared && prepared.cursor,
               "long downtime creates one catch-up pulse");
        if (prepared.cursor) {
            expect(prepared.cursor->generation == 0,
                   "missed intervals do not synthesize generations");
            const auto snapshot = fixture.tasks.find(prepared.cursor->snapshot_task_id);
            if (snapshot && snapshot->result) {
                const auto capsule = Json::parse(snapshot->result->output);
                const auto facts = Json::parse(capsule.at("content").get<std::string>());
                expect(facts.at("pulse").at("lateness_ms")
                           == std::chrono::duration_cast<std::chrono::milliseconds>(30h).count(),
                       "catch-up context records exact lateness");
            }
        }
        expect(row_count(database.generic, "tasks") == tasks_before + 2,
               "long downtime still creates exactly one snapshot/current pair");
        for (int iteration = 0; iteration < 20; ++iteration)
            expect(pulse.observe().result == PulseResult::waiting,
                   "catch-up does not burst additional pulses");
        expect(row_count(database.generic, "tasks") == tasks_before + 2,
               "repeated catch-up observations create no backlog storm");
    }
}

void test_recovery_boundaries()
{
    {
        TemporaryDatabases database("freeze-reopen");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        std::string recovered_id;
        {
            AutonomousCognitionPulseStore sidecar(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto seeded = pulse.seed(predecessor_id);
            if (!seeded.cursor) throw std::runtime_error("freeze seed failed");
            fixture.now = gaudere::work::TimePoint{
                std::chrono::milliseconds{seeded.cursor->due_at_ms}};
            auto frozen = *seeded.cursor;
            ++frozen.revision;
            frozen.state = AutonomousCognitionPulseState::preparing;
            frozen.observed_at_ms = seeded.cursor->due_at_ms;
            const auto write = sidecar.replace(*seeded.cursor, frozen);
            expect(write.result == AutonomousCognitionPulseStoreResult::accepted,
                   "synthetic crash freezes due observation first");
        }
        {
            AutonomousCognitionPulseStore reopened(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                reopened, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto recovered = pulse.observe();
            expect(recovered.result == PulseResult::prepared && recovered.cursor,
                   "reopen after frozen observation completes same generation");
            if (recovered.cursor) recovered_id = recovered.cursor->current_task_id;
        }
        expect(!recovered_id.empty(), "freeze-reopen creates one current cognition id");
    }

    {
        TemporaryDatabases database("snapshot-reopen");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        std::string snapshot_id;
        std::string current_id;
        {
            AutonomousCognitionPulseStore sidecar(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto seeded = pulse.seed(predecessor_id);
            if (!seeded.cursor) throw std::runtime_error("snapshot reopen seed failed");
            fixture.now = gaudere::work::TimePoint{
                std::chrono::milliseconds{seeded.cursor->due_at_ms}};
            const auto prepared = pulse.observe();
            if (!prepared.cursor) throw std::runtime_error("snapshot reopen prepare failed");
            snapshot_id = prepared.cursor->snapshot_task_id;
            current_id = prepared.cursor->current_task_id;
            auto preparing = *prepared.cursor;
            ++preparing.revision;
            preparing.state = AutonomousCognitionPulseState::preparing;
            preparing.snapshot_task_id.clear();
            preparing.current_task_id.clear();
            const auto write = sidecar.replace(*prepared.cursor, preparing);
            expect(write.result == AutonomousCognitionPulseStoreResult::accepted,
                   "synthetic crash rewinds only sidecar commit point");
            execute_sql(database.generic,
                "DELETE FROM tasks WHERE id='" + current_id + "'");
            expect(fixture.tasks.find(snapshot_id).has_value(),
                   "snapshot remains durable across synthetic crash");
            expect(!fixture.tasks.find(current_id),
                   "current Task absent at synthetic after-snapshot crash point");
        }
        {
            AutonomousCognitionPulseStore reopened(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                reopened, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto recovered = pulse.observe();
            expect(recovered.result == PulseResult::prepared && recovered.cursor,
                   "reopen after snapshot recreates deterministic current Task");
            if (recovered.cursor) {
                expect(recovered.cursor->snapshot_task_id == snapshot_id,
                       "snapshot identity survives reopen");
                expect(recovered.cursor->current_task_id == current_id,
                       "current cognition identity is reconstructed exactly");
            }
        }
    }

    {
        TemporaryDatabases database("claim-reopen");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        std::string snapshot_id;
        std::string current_id;
        {
            AutonomousCognitionPulseStore sidecar(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto seeded = pulse.seed(predecessor_id);
            if (!seeded.cursor) throw std::runtime_error("claim reopen seed failed");
            fixture.now = gaudere::work::TimePoint{
                std::chrono::milliseconds{seeded.cursor->due_at_ms}};
            const auto prepared = pulse.observe();
            if (!prepared.cursor) throw std::runtime_error("claim reopen prepare failed");
            snapshot_id = prepared.cursor->snapshot_task_id;
            current_id = prepared.cursor->current_task_id;
            auto preparing = *prepared.cursor;
            ++preparing.revision;
            preparing.state = AutonomousCognitionPulseState::preparing;
            preparing.snapshot_task_id.clear();
            preparing.current_task_id.clear();
            const auto write = sidecar.replace(*prepared.cursor, preparing);
            expect(write.result == AutonomousCognitionPulseStoreResult::accepted,
                   "synthetic crash occurs after current Task but before cursor commit");
        }
        {
            AutonomousCognitionPulseStore reopened(database.sidecar.string());
            AutonomousCognitionPulse pulse(
                reopened, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
                [&fixture] { return fixture.now; }, true);
            const auto tasks_before = row_count(database.generic, "tasks");
            const auto recovered = pulse.observe();
            expect(recovered.result == PulseResult::prepared && recovered.cursor,
                   "reopen after current Task accepts exact duplicates");
            if (recovered.cursor) {
                expect(recovered.cursor->snapshot_task_id == snapshot_id,
                       "snapshot id remains exact after claim reopen");
                expect(recovered.cursor->current_task_id == current_id,
                       "current id remains exact after claim reopen");
            }
            expect(row_count(database.generic, "tasks") == tasks_before,
                   "claim reopen creates no duplicate Tasks");
        }
    }
}

void test_stale_recovery_and_sidecar_version_fail_closed()
{
    {
        TemporaryDatabases database("stale");
        Fixture fixture(database.generic);
        const auto predecessor_id = fixture.make_seed_current();
        AutonomousCognitionPulseStore sidecar(database.sidecar.string());
        AutonomousCognitionPulse pulse(
            sidecar, fixture.tasks, fixture.budgets, fixture.wakes, fixture.runtime,
            [&fixture] { return fixture.now; }, true);
        const auto seeded = pulse.seed(predecessor_id);
        if (!seeded.cursor) throw std::runtime_error("stale seed failed");
        fixture.now = gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}};
        auto frozen = *seeded.cursor;
        ++frozen.revision;
        frozen.state = AutonomousCognitionPulseState::preparing;
        frozen.observed_at_ms = seeded.cursor->due_at_ms;
        if (sidecar.replace(*seeded.cursor, frozen).result
            != AutonomousCognitionPulseStoreResult::accepted)
            throw std::runtime_error("could not freeze stale fixture");
        fixture.now += 16min;
        const auto blocked = pulse.observe();
        expect(blocked.result == PulseResult::blocked && blocked.cursor
                   && blocked.cursor->state == AutonomousCognitionPulseState::blocked,
               "stale crash recovery blocks instead of fabricating fresh observation");
        expect(row_count(database.generic, "tasks") == 3,
               "stale recovery creates no autonomous snapshot/current Tasks");
    }

    {
        TemporaryDatabases database("future-schema");
        sqlite3* raw = nullptr;
        if (sqlite3_open(database.sidecar.c_str(), &raw) != SQLITE_OK)
            throw std::runtime_error("could not create future sidecar fixture");
        sqlite3_exec(raw, "PRAGMA user_version=2;", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
        bool rejected = false;
        try {
            AutonomousCognitionPulseStore sidecar(database.sidecar.string());
            (void)sidecar;
        } catch (const std::exception&) {
            rejected = true;
        }
        expect(rejected, "future sidecar schema fails closed");
        expect(user_version(database.sidecar) == 2,
               "future sidecar is not silently recreated or downgraded");
    }

    {
        TemporaryDatabases database("unversioned-corrupt");
        sqlite3* raw = nullptr;
        if (sqlite3_open(database.sidecar.c_str(), &raw) != SQLITE_OK)
            throw std::runtime_error("could not create corrupt sidecar fixture");
        sqlite3_exec(raw, "CREATE TABLE unexpected(x INTEGER);", nullptr, nullptr, nullptr);
        sqlite3_close(raw);
        bool rejected = false;
        try {
            AutonomousCognitionPulseStore sidecar(database.sidecar.string());
            (void)sidecar;
        } catch (const std::exception&) {
            rejected = true;
        }
        expect(rejected, "non-empty unversioned sidecar fails closed");
        expect(table_set(database.sidecar) == "unexpected",
               "corrupt unversioned sidecar is preserved for evidence");
    }
}

} // namespace

int main()
{
    try {
        test_sha_and_sidecar_isolation();
        test_due_prepare_idempotence_and_continue_settlement();
        test_stop_quiescence_and_terminal_blocking();
        test_budget_time_and_catchup_guards();
        test_recovery_boundaries();
        test_stale_recovery_and_sidecar_version_fail_closed();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " autonomous cognition pulse assertion(s) failed\n";
        return 1;
    }
    std::cout << "Autonomous cognition pulse provider-free proof: PASS\n";
    return 0;
}
