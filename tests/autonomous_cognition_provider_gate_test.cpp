#include "AutonomousCognitionProviderGate.hpp"
#include "CurrentCognitionCycle.hpp"
#include "OpenAIBudget.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
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
using GateResult = AutonomousCognitionProviderGateResult;

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
        const auto token = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path()
            / ("gaudere-autonomous-provider-gate-" + label + "-" + token + ".db");
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

std::int64_t query_int(const std::filesystem::path& path, const std::string& sql)
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
    if (sqlite3_step(statement) != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error("could not execute sqlite query");
    }
    const auto result = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return result;
}

std::int64_t rows(const std::filesystem::path& path, const char* table)
{
    return query_int(path, "SELECT COUNT(*) FROM " + std::string(table));
}

std::int64_t millis(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string decision_continue(const std::string& objective)
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "continue"},
                {"reason", "Current durable evidence supports another bounded step."},
                {"objective", objective}}.dump();
}

std::string snapshot_request(const std::string& content, const std::string& ref)
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
    task.id = "cognition.resume-after-wake.v0:provider-gate-bootstrap";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "provider-free autonomous provider gate bootstrap";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("Create a canonical current-cognition predecessor."), {}, {}};
    return task;
}

struct Prepared {
    AutonomousCognitionPulseCursor cursor;
    std::string predecessor_id;
    std::string snapshot_id;
    std::string current_id;
};

struct Fixture {
    explicit Fixture(const std::string& label)
        : temporary(label),
          tasks(temporary.path.string()),
          actions(temporary.path.string()),
          budgets(temporary.path.string()),
          now(gaudere::work::TimePoint{std::chrono::milliseconds{2000000000000LL}}),
          runtime(tasks, [this] { return now; })
    {
        runtime.recover();
    }

    Prepared prepare()
    {
        const auto bootstrap = bootstrap_resume_task();
        tasks.save(bootstrap);

        ResumeContextSnapshotRecorder recorder(
            tasks, runtime, [this] { return now; });
        const auto seed_snapshot = recorder.record(snapshot_request(
            "Provider gate seed context.", "provider-gate-seed"));
        if (!seed_snapshot.task)
            throw std::runtime_error("could not create seed snapshot");

        CurrentCognitionCycle cycle(tasks, runtime, [this] { return now; }, true);
        const auto seed_claim = cycle.claim(bootstrap.id, seed_snapshot.task->id);
        if (!seed_claim.task)
            throw std::runtime_error("could not create predecessor cognition");
        auto predecessor = *seed_claim.task;
        predecessor.attempts_started = 1;
        predecessor.status = TaskStatus::succeeded;
        predecessor.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type,
            decision_continue("Let the autonomous pulse choose the next bounded thought."),
            {}, {}};
        tasks.save(predecessor);

        now += 1min;
        ResumeContextSnapshotRecorder current_recorder(
            tasks, runtime, [this] { return now; });
        // Deliberately repeat the linkage marker inside caller-controlled evidence.
        // Extraction must remain bound to the canonical prompt marker.
        const auto current_snapshot = current_recorder.record(snapshot_request(
            "Current pulse facts; Durable cognition linkage JSON:\n is data here.",
            "provider-gate-current"));
        if (!current_snapshot.task)
            throw std::runtime_error("could not create current snapshot");
        const auto current_claim = cycle.claim(predecessor.id, current_snapshot.task->id);
        if (!current_claim.task)
            throw std::runtime_error("could not create current cognition claim");

        AutonomousCognitionPulseCursor cursor;
        cursor.revision = 4;
        cursor.generation = 2;
        cursor.state = AutonomousCognitionPulseState::prepared;
        cursor.predecessor_task_id = predecessor.id;
        cursor.predecessor_result_sha256 = sha256_hex(predecessor.result->output);
        cursor.anchor_at_ms = millis(now - 6h);
        cursor.due_at_ms = millis(now);
        cursor.observed_at_ms = millis(now);
        cursor.snapshot_task_id = current_snapshot.task->id;
        cursor.current_task_id = current_claim.task->id;
        if (!valid_autonomous_cognition_pulse_cursor(cursor))
            throw std::runtime_error("fixture cursor is non-canonical");

        return {cursor, predecessor.id, current_snapshot.task->id,
                current_claim.task->id};
    }

    AutonomousCognitionProviderGate gate()
    {
        return AutonomousCognitionProviderGate(
            temporary.path.string(), tasks, budgets, actions,
            [this] { return now; });
    }

    gaudere::budget::ConsumeResult consume(
        const std::string& key, const gaudere::work::TimePoint when)
    {
        return budgets.consume(std::string{openai_budget_scope()}, key, when,
                               openai_bootstrap_budget_policy());
    }

    TemporaryDatabase temporary;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::work::TimePoint now;
    gaudere::work::Runtime runtime;
};

void test_eligible_is_exact_and_read_only()
{
    Fixture fixture("eligible");
    const auto prepared = fixture.prepare();
    const auto tasks_before = rows(fixture.temporary.path, "tasks");
    const auto actions_before = rows(fixture.temporary.path, "actions");
    const auto budget_before = rows(fixture.temporary.path, "budget_consumptions");

    const auto result = fixture.gate().evaluate(prepared.cursor);
    expect(result.result == GateResult::eligible, "canonical prepared cursor is eligible");
    expect(result.task_id && *result.task_id == prepared.current_id,
           "eligible gate returns exact pulse Task id");
    expect(!result.retry_at, "eligible result has no retry deadline");
    expect(rows(fixture.temporary.path, "tasks") == tasks_before,
           "gate does not mutate Tasks");
    expect(rows(fixture.temporary.path, "actions") == actions_before,
           "gate does not create provider Actions");
    expect(rows(fixture.temporary.path, "budget_consumptions") == budget_before,
           "gate does not consume provider budget");
}

void test_nonprepared_states_do_not_grant_authority()
{
    Fixture fixture("states");
    const auto prepared = fixture.prepare();

    auto idle = prepared.cursor;
    idle.state = AutonomousCognitionPulseState::idle;
    idle.observed_at_ms.reset();
    idle.snapshot_task_id.clear();
    idle.current_task_id.clear();
    expect(valid_autonomous_cognition_pulse_cursor(idle), "idle fixture is canonical");
    expect(fixture.gate().evaluate(idle).result == GateResult::waiting,
           "idle pulse waits");

    auto quiescent = idle;
    quiescent.state = AutonomousCognitionPulseState::quiescent;
    expect(fixture.gate().evaluate(quiescent).result == GateResult::waiting,
           "quiescent pulse waits");

    auto preparing = idle;
    preparing.state = AutonomousCognitionPulseState::preparing;
    preparing.observed_at_ms = prepared.cursor.observed_at_ms;
    expect(valid_autonomous_cognition_pulse_cursor(preparing),
           "preparing fixture is canonical");
    expect(fixture.gate().evaluate(preparing).result == GateResult::waiting,
           "preparing pulse keeps preparation authority");

    auto blocked_cursor = prepared.cursor;
    blocked_cursor.state = AutonomousCognitionPulseState::blocked;
    blocked_cursor.blocked_reason = "fixture block";
    expect(fixture.gate().evaluate(blocked_cursor).result == GateResult::blocked,
           "blocked pulse cannot grant provider authority");
}

void test_lineage_and_definition_drift_block()
{
    {
        Fixture fixture("missing-task");
        auto prepared = fixture.prepare();
        prepared.cursor.current_task_id = "cognition.current.v0:missing";
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "wrong cursor Task id blocks");
    }
    {
        Fixture fixture("snapshot-drift");
        auto prepared = fixture.prepare();
        prepared.cursor.snapshot_task_id = "continuity.resume-context-snapshot.v1:missing";
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "snapshot lineage drift blocks");
    }
    {
        Fixture fixture("predecessor-drift");
        auto prepared = fixture.prepare();
        prepared.cursor.predecessor_task_id = "cognition.current.v0:missing";
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "predecessor lineage drift blocks");
    }
    {
        Fixture fixture("task-malformed");
        const auto prepared = fixture.prepare();
        auto task = *fixture.tasks.find(prepared.current_id);
        task.input += "x";
        fixture.tasks.save(task);
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "malformed current cognition blocks");
    }
    {
        Fixture fixture("snapshot-malformed");
        const auto prepared = fixture.prepare();
        auto snapshot = *fixture.tasks.find(prepared.snapshot_id);
        snapshot.result->output += "x";
        fixture.tasks.save(snapshot);
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "durable snapshot corruption blocks");
    }
    {
        Fixture fixture("predecessor-hash");
        const auto prepared = fixture.prepare();
        auto predecessor = *fixture.tasks.find(prepared.predecessor_id);
        predecessor.result->output = decision_continue("A different durable objective.");
        fixture.tasks.save(predecessor);
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "predecessor result drift blocks");
    }
}

void test_singleton_and_execution_ownership_block()
{
    {
        Fixture fixture("singleton");
        const auto prepared = fixture.prepare();
        auto duplicate = *fixture.tasks.find(prepared.current_id);
        duplicate.id = "cognition.current.v0:second-nonterminal";
        duplicate.idempotency_key = duplicate.id;
        fixture.tasks.save(duplicate);
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "second non-terminal current cognition blocks authority");
    }
    {
        Fixture fixture("running");
        const auto prepared = fixture.prepare();
        auto running = *fixture.tasks.find(prepared.current_id);
        running.attempts_started = 1;
        running.status = TaskStatus::running;
        running.lease = gaudere::work::Lease{"another-owner", fixture.now + 1min};
        fixture.tasks.save(running);
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "running current cognition blocks ambiguous ownership");
    }
}

void test_freshness_blocks_stale_or_future_context()
{
    {
        Fixture fixture("stale");
        const auto prepared = fixture.prepare();
        fixture.now += current_cognition_max_snapshot_age + 1ms;
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "stale prepared context blocks provider authority");
    }
    {
        Fixture fixture("future");
        const auto prepared = fixture.prepare();
        fixture.now -= 1ms;
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "clock before frozen context blocks provider authority");
    }
}

void test_budget_states_are_observational()
{
    const auto policy = openai_bootstrap_budget_policy();
    {
        Fixture fixture("cooldown");
        const auto prepared = fixture.prepare();
        expect(fixture.consume("cooldown", fixture.now - 5min)
                   == gaudere::budget::ConsumeResult::accepted,
               "cooldown fixture consumption accepted");
        const auto before = rows(fixture.temporary.path, "budget_consumptions");
        const auto result = fixture.gate().evaluate(prepared.cursor);
        expect(result.result == GateResult::waiting, "cooldown waits");
        expect(result.retry_at && *result.retry_at == fixture.now + 10min,
               "cooldown exposes exact retry deadline");
        expect(rows(fixture.temporary.path, "budget_consumptions") == before,
               "cooldown gate does not consume another permit");
    }
    {
        Fixture fixture("window");
        const auto prepared = fixture.prepare();
        for (int index = 0; index < 4; ++index) {
            const auto when = fixture.now - (20h - std::chrono::hours{index * 5});
            expect(fixture.consume("window-" + std::to_string(index), when)
                       == gaudere::budget::ConsumeResult::accepted,
                   "window fixture consumption accepted");
        }
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::waiting,
               "rolling-window exhaustion waits without call");
    }
    {
        Fixture fixture("total");
        const auto prepared = fixture.prepare();
        int key = 0;
        for (int group = 0; group < 3; ++group) {
            const auto base = fixture.now - 80h + std::chrono::hours{group * 30};
            for (int offset = 0; offset < 4; ++offset) {
                expect(fixture.consume("total-" + std::to_string(key++),
                                       base + std::chrono::hours{offset})
                           == gaudere::budget::ConsumeResult::accepted,
                       "total fixture consumption accepted");
            }
        }
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::dormant,
               "lifetime exhaustion makes autonomous provider dormant");
    }
    {
        Fixture fixture("clock-rollback");
        const auto prepared = fixture.prepare();
        expect(fixture.consume("future-budget", fixture.now + 1min)
                   == gaudere::budget::ConsumeResult::accepted,
               "future budget fixture consumption accepted");
        expect(fixture.gate().evaluate(prepared.cursor).result == GateResult::blocked,
               "budget clock rollback blocks authority");
    }
    expect(policy.max_total == 12 && policy.max_in_window == 4,
           "proof uses production OpenAI budget policy");
}

void test_existing_action_forbids_replay()
{
    Fixture fixture("action");
    const auto prepared = fixture.prepare();
    gaudere::scheduling::wake::Action action;
    action.id = std::string{openai_budget_scope()} + ":" + prepared.current_id;
    action.idempotency_key = action.id;
    action.critical = true;
    fixture.actions.save(action);
    const auto budget_before = rows(fixture.temporary.path, "budget_consumptions");
    const auto result = fixture.gate().evaluate(prepared.cursor);
    expect(result.result == GateResult::blocked,
           "existing provider Action blocks automatic replay");
    expect(rows(fixture.temporary.path, "budget_consumptions") == budget_before,
           "existing Action preflight consumes no provider permit");
}

void test_selector_unavailable_fails_closed()
{
    Fixture fixture("selector-unavailable");
    const auto prepared = fixture.prepare();
    AutonomousCognitionProviderGate gate(
        fixture.temporary.path.string() + ".missing",
        fixture.tasks, fixture.budgets, fixture.actions,
        [&fixture] { return fixture.now; });
    expect(gate.evaluate(prepared.cursor).result == GateResult::unavailable,
           "unavailable read-only singleton selector fails closed");
}

} // namespace

int main()
{
    test_eligible_is_exact_and_read_only();
    test_nonprepared_states_do_not_grant_authority();
    test_lineage_and_definition_drift_block();
    test_singleton_and_execution_ownership_block();
    test_freshness_blocks_stale_or_future_context();
    test_budget_states_are_observational();
    test_existing_action_forbids_replay();
    test_selector_unavailable_fails_closed();

    if (failures != 0) {
        std::cerr << failures << " autonomous provider gate checks failed\n";
        return 1;
    }
    std::cout << "autonomous_cognition_provider_gate=PASS\n"
              << "provider_effects=0\n"
              << "service_wiring=0\n";
    return 0;
}
