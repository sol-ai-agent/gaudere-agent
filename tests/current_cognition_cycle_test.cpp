#include "CurrentCognitionCycle.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
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
using Task = gaudere::work::Task;

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
            / ("gaudere-current-cognition-" + std::move(label) + "-"
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
        != SQLITE_OK) throw std::runtime_error("could not open sqlite database");
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("could not prepare row count");
    }
    const auto step = sqlite3_step(statement);
    const auto result = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (result < 0) throw std::runtime_error("could not read row count");
    return result;
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
                {"reason", "No useful bounded objective remains in the supplied evidence."}}
        .dump();
}

Task succeeded_decision_task(const std::string& id,
                             const std::string& kind,
                             const std::string& output)
{
    Task task;
    task.id = id;
    task.idempotency_key = id;
    task.kind = kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "historical cognition fixture";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type, output, {}, {}};
    return task;
}

std::string snapshot_request(const std::string& content,
                             const std::string& ref = "current-cycle-proof")
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/markdown; charset=utf-8"},
        {"content", content},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", ref},
            {"sha256", std::string(64, '0')}
        }})}
    }.dump();
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path)
        : tasks(path.string()), actions(path.string()), budgets(path.string()),
          wakes(path.string()), now(gaudere::work::TimePoint{1000000ms}),
          runtime(tasks, [this] { return now; }),
          recorder(tasks, runtime, [this] { return now; }),
          cognition(tasks, runtime, [this] { return now; }, true)
    {
        runtime.recover();
    }

    std::string record(const std::string& content,
                       const std::string& ref = "current-cycle-proof")
    {
        const auto result = recorder.record(snapshot_request(content, ref));
        if ((result.result != ResumeContextSnapshotRecordResult::accepted
             && result.result != ResumeContextSnapshotRecordResult::duplicate)
            || !result.task) {
            throw std::runtime_error("snapshot record failed: " + result.detail);
        }
        return result.task->id;
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint now;
    gaudere::work::Runtime runtime;
    ResumeContextSnapshotRecorder recorder;
    CurrentCognitionCycle cognition;
};

void expect_no_external_effects(const TemporaryDatabase& database,
                                const std::string& label)
{
    expect(count_rows(database.path, "actions") == 0, label + ": no Actions");
    expect(count_rows(database.path, "budget_consumptions") == 0,
           label + ": no provider budget consumption");
    expect(count_rows(database.path, "wake_intents") == 0,
           label + ": no WakeIntent created");
}

void test_repeatable_chain_and_idempotence()
{
    TemporaryDatabase database("chain");
    Fixture fixture(database.path);
    const auto predecessor = succeeded_decision_task(
        "cognition.resume-after-wake.v0:bootstrap",
        resume_after_wake_task_kind,
        decision_continue("Replace the stale wake objective with current durable facts."));
    fixture.tasks.save(predecessor);

    const auto first_snapshot = fixture.record(
        "Provider call #6 happened exactly once and failed closed after the provider "
        "boundary because the v1 transient prompt omitted the required output contract. "
        "PR #108 fixed that defect provider-free. Provider total is 6/12.");
    fixture.now += 1min;
    const auto first = fixture.cognition.claim(predecessor.id, first_snapshot);
    expect(first.result == CurrentCognitionClaimResult::accepted && first.task,
           "first current cognition claim accepted");
    if (!first.task) return;
    const auto first_id = first.task->id;
    expect(first_id.rfind(current_cognition_task_prefix, 0) == 0,
           "current cognition id uses canonical prefix");
    expect(first_id.size() == std::string(current_cognition_task_prefix).size() + 64,
           "current cognition identity contains SHA-256 digest");
    expect(first.task->kind == current_cognition_task_kind,
           "current cognition kind is canonical");
    expect(first.task->input_content_type == "text/plain; charset=utf-8",
           "current cognition prompt is directly provider-compatible text");
    expect(first.task->input.find("Return exactly one JSON object") != std::string::npos,
           "current cognition prompt contains exact output contract");
    expect(first.task->input.find("advances Gaudere's own continuity") != std::string::npos,
           "current cognition prompt asks for a self-directed bounded objective");
    expect(first.task->input.find(predecessor.id) != std::string::npos,
           "prompt preserves predecessor identity");
    expect(first.task->input.find("Provider call #6 happened exactly once")
               != std::string::npos,
           "prompt preserves fresh snapshot evidence");
    expect(first.task->input.find("grants no shell, tool, network, successor")
               != std::string::npos,
           "prompt explicitly grants no action authority");

    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto duplicate = fixture.cognition.claim(predecessor.id, first_snapshot);
        expect(duplicate.result == CurrentCognitionClaimResult::duplicate
                   && duplicate.task && duplicate.task->id == first_id,
               "100 repeated claims remain idempotent");
    }

    fixture.now += 30min;
    const auto aged_reopen = fixture.cognition.claim(predecessor.id, first_snapshot);
    expect(aged_reopen.result == CurrentCognitionClaimResult::duplicate
               && aged_reopen.task && aged_reopen.task->id == first_id,
           "existing cognition remains identical after freshness window");

    const auto second_snapshot = fixture.record(
        "The previous current cognition Task is now durably complete; choose the next "
        "bounded continuity objective from this later snapshot.",
        "current-cycle-proof-2");
    fixture.now += 1min;
    const auto second = fixture.cognition.claim(predecessor.id, second_snapshot);
    expect(second.result == CurrentCognitionClaimResult::accepted && second.task,
           "later fresh snapshot creates a distinct cognition cycle");
    expect(second.task && second.task->id != first_id,
           "later snapshot changes deterministic cognition identity");

    auto completed_first = *first.task;
    completed_first.attempts_started = 1;
    completed_first.status = gaudere::work::TaskStatus::succeeded;
    completed_first.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("Record and prove the next current cognition cycle."), {}, {}};
    fixture.tasks.save(completed_first);
    fixture.now += 1min;
    const auto third_snapshot = fixture.record(
        "A successful cognition.current.v0 predecessor now exists; chain from it rather "
        "than from the historical wake-resume decision.",
        "current-cycle-proof-3");
    fixture.now += 1min;
    const auto chained = fixture.cognition.claim(first_id, third_snapshot);
    expect(chained.result == CurrentCognitionClaimResult::accepted && chained.task,
           "successful current cognition can become next predecessor");
    expect(chained.task && chained.task->id != first_id,
           "chained cognition has its own deterministic identity");

    expect_no_external_effects(database, "repeatable chain");
}

void test_invalid_predecessors_and_snapshots()
{
    TemporaryDatabase database("invalid");
    Fixture fixture(database.path);
    const auto snapshot = fixture.record("fresh valid context");

    const auto missing = fixture.cognition.claim("missing-predecessor", snapshot);
    expect(missing.result == CurrentCognitionClaimResult::predecessor_not_found,
           "missing predecessor fails closed");

    auto failed = succeeded_decision_task(
        "failed-predecessor", resume_after_wake_task_kind, decision_stop());
    failed.status = gaudere::work::TaskStatus::failed;
    failed.result = gaudere::work::TaskResult{
        "application/vnd.gaudere.failure+json", "{}", "failed", "fixture"};
    fixture.tasks.save(failed);
    expect(fixture.cognition.claim(failed.id, snapshot).result
               == CurrentCognitionClaimResult::ineligible,
           "failed predecessor is ineligible");

    auto malformed = succeeded_decision_task(
        "malformed-predecessor", resume_after_wake_task_kind,
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"continue\",\"reason\":\"x\",\"reason\":\"y\","
        "\"objective\":\"z\"}");
    fixture.tasks.save(malformed);
    expect(fixture.cognition.claim(malformed.id, snapshot).result
               == CurrentCognitionClaimResult::ineligible,
           "duplicate-key predecessor decision is rejected");

    const auto valid = succeeded_decision_task(
        "valid-predecessor", resume_after_wake_task_kind,
        decision_continue("Use fresh context."));
    fixture.tasks.save(valid);
    fixture.now += 16min;
    expect(fixture.cognition.claim(valid.id, snapshot).result
               == CurrentCognitionClaimResult::stale,
           "stale new-cycle snapshot is rejected");

    fixture.now -= 20min;
    const auto future_snapshot = fixture.record("capture that becomes future to clock");
    fixture.now -= 1min;
    expect(fixture.cognition.claim(valid.id, future_snapshot).result
               == CurrentCognitionClaimResult::ineligible,
           "clock rollback before snapshot capture fails closed");

    CurrentCognitionCycle disabled(
        fixture.tasks, fixture.runtime, [&fixture] { return fixture.now; }, false);
    expect(disabled.claim(valid.id, future_snapshot).result
               == CurrentCognitionClaimResult::disabled,
           "current cognition remains disabled by default");

    expect_no_external_effects(database, "invalid inputs");
}

void test_definition_collision_fails_closed()
{
    TemporaryDatabase database("collision");
    Fixture fixture(database.path);
    const auto predecessor = succeeded_decision_task(
        "collision-predecessor", resume_after_wake_task_kind,
        decision_continue("Prove collision handling."));
    fixture.tasks.save(predecessor);
    const auto snapshot = fixture.record("collision fixture context");
    fixture.now += 1min;
    const auto accepted = fixture.cognition.claim(predecessor.id, snapshot);
    expect(accepted.result == CurrentCognitionClaimResult::accepted && accepted.task,
           "collision fixture initial claim accepted");
    if (!accepted.task) return;

    auto corrupt = *accepted.task;
    corrupt.input += "CORRUPTION";
    fixture.tasks.save(corrupt);
    const auto collided = fixture.cognition.claim(predecessor.id, snapshot);
    expect(collided.result == CurrentCognitionClaimResult::conflict,
           "same deterministic id with changed definition fails closed");
    expect_no_external_effects(database, "definition collision");
}

} // namespace

int main()
{
    try {
        test_repeatable_chain_and_idempotence();
        test_invalid_predecessors_and_snapshots();
        test_definition_collision_fails_closed();
        if (failures != 0) {
            std::cerr << "current cognition cycle: FAIL count=" << failures << '\n';
            return 1;
        }
        std::cout << "current cognition cycle provider-free: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "current cognition cycle: ERROR: " << error.what() << '\n';
        return 2;
    }
}
