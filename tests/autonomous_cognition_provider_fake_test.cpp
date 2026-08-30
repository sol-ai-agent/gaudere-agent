#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "CurrentCognitionCycle.hpp"
#include "CurrentCognitionHandler.hpp"
#include "OpenAIBudget.hpp"
#include "ProviderTaskHandler.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;
using namespace std::chrono_literals;
using GateResult = AutonomousCognitionProviderGateResult;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using ActionStatus = gaudere::scheduling::wake::ActionStatus;
using EffectResult = gaudere::scheduling::wake::EffectResult;

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
        state = root / ("gaudere-autonomous-provider-fake-state-" + label + "-"
                        + token + ".db");
        sidecar = root / ("gaudere-autonomous-provider-fake-pulse-" + label + "-"
                          + token + ".db");
    }

    ~TemporaryDatabases()
    {
        remove_database(state);
        remove_database(sidecar);
    }

    static void remove_database(const std::filesystem::path& path)
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path state;
    std::filesystem::path sidecar;
};

std::int64_t row_count(const std::filesystem::path& path, const char* table)
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
    const auto value = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (value < 0) throw std::runtime_error("could not read row count");
    return value;
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string decision_continue(const std::string& objective)
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "continue"},
                {"reason", "Fresh durable evidence supports one bounded autonomy step."},
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

Task bootstrap_resume_task(const std::string& label)
{
    Task task;
    task.id = "cognition.resume-after-wake.v0:autonomous-provider-fake-" + label;
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "autonomous provider fake bootstrap";
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

class FakeOpenAIProvider final : public Provider {
public:
    std::string_view name() const noexcept override { return "openai.responses"; }

    ProviderResult invoke(const ProviderRequest& request) override
    {
        ++calls;
        last_request = request;
        return next_result;
    }

    int calls = 0;
    ProviderResult next_result;
    std::optional<ProviderRequest> last_request;
};

struct Harness {
    Harness(const std::filesystem::path& state,
            const std::filesystem::path& sidecar,
            const std::int64_t start_ms)
        : state_path(state.string()),
          tasks(state_path), actions(state_path), budgets(state_path), wakes(state_path),
          pulse_store(sidecar.string()),
          work_now(gaudere::work::TimePoint{std::chrono::milliseconds{start_ms}}),
          action_now(gaudere::scheduling::wake::TimePoint{
              std::chrono::milliseconds{start_ms}}),
          budget_now(gaudere::budget::TimePoint{std::chrono::milliseconds{start_ms}}),
          work_runtime(tasks, [this] { return work_now; }),
          action_runtime(actions, [this] { return action_now; }),
          pulse(pulse_store, tasks, budgets, wakes, work_runtime,
                [this] { return work_now; }, true),
          provider_handler(action_runtime, actions, provider, budgets,
                           openai_bootstrap_budget_policy(),
                           [this] { return budget_now; }),
          cognition_handler(provider_handler),
          executor(work_runtime, tasks),
          gate(state_path, tasks, budgets, actions, [this] { return work_now; })
    {
        work_runtime.recover();
        action_runtime.recover();
    }

    void set_now(const gaudere::work::TimePoint value)
    {
        work_now = value;
        const auto duration = work_now.time_since_epoch();
        action_now = gaudere::scheduling::wake::TimePoint{duration};
        budget_now = gaudere::budget::TimePoint{duration};
    }

    void advance(const std::chrono::milliseconds delta)
    {
        set_now(work_now + delta);
    }

    std::string make_seed_current(const std::string& label)
    {
        const auto bootstrap = bootstrap_resume_task(label);
        tasks.save(bootstrap);

        ResumeContextSnapshotRecorder recorder(
            tasks, work_runtime, [this] { return work_now; });
        const auto content = "Fresh seed evidence for autonomous provider fake " + label;
        const auto snapshot = recorder.record(snapshot_request(
            content, "autonomous-provider-fake-seed-" + label));
        if ((snapshot.result != ResumeContextSnapshotRecordResult::accepted
             && snapshot.result != ResumeContextSnapshotRecordResult::duplicate)
            || !snapshot.task) {
            throw std::runtime_error("could not create seed snapshot: " + snapshot.detail);
        }

        CurrentCognitionCycle cycle(
            tasks, work_runtime, [this] { return work_now; }, true);
        const auto claim = cycle.claim(bootstrap.id, snapshot.task->id);
        if ((claim.result != CurrentCognitionClaimResult::accepted
             && claim.result != CurrentCognitionClaimResult::duplicate)
            || !claim.task) {
            throw std::runtime_error("could not create seed current cognition: "
                                     + claim.detail);
        }

        auto completed = *claim.task;
        completed.attempts_started = 1;
        completed.status = TaskStatus::succeeded;
        completed.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type,
            decision_continue("Prepare one pulse-owned fresh cognition opportunity."),
            {}, {}};
        tasks.save(completed);
        if (!valid_current_cognition_task(completed))
            throw std::runtime_error("seed current cognition is non-canonical");
        return completed.id;
    }

    AutonomousCognitionPulseCursor prepare_pulse(const std::string& label)
    {
        const auto predecessor = make_seed_current(label);
        const auto seeded = pulse.seed(predecessor);
        if (seeded.result != PulseResult::seeded || !seeded.cursor)
            throw std::runtime_error("could not seed autonomous pulse");
        set_now(gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}});
        const auto prepared = pulse.observe();
        if (prepared.result != PulseResult::prepared || !prepared.cursor
            || !prepared.task) {
            throw std::runtime_error("could not prepare autonomous pulse: "
                                     + prepared.detail);
        }
        return *prepared.cursor;
    }

    std::string state_path;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    AutonomousCognitionPulseStore pulse_store;
    gaudere::work::TimePoint work_now;
    gaudere::scheduling::wake::TimePoint action_now;
    gaudere::budget::TimePoint budget_now;
    gaudere::work::Runtime work_runtime;
    gaudere::scheduling::wake::Runtime action_runtime;
    AutonomousCognitionPulse pulse;
    FakeOpenAIProvider provider;
    ProviderTaskHandler provider_handler;
    CurrentCognitionHandler cognition_handler;
    TaskExecutor executor;
    AutonomousCognitionProviderGate gate;
};

std::string action_id(const std::string& task_id)
{
    return std::string{openai_budget_scope()} + ":" + task_id;
}

void expect_one_provider_effect(Harness& harness,
                                const TemporaryDatabases& database,
                                const std::string& task_id,
                                const std::string& label)
{
    const auto action = harness.actions.find(action_id(task_id));
    expect(action.has_value(), label + ": provider Action exists");
    expect(row_count(database.state, "actions") == 1,
           label + ": exactly one Action row exists");
    expect(row_count(database.state, "budget_consumptions") == 1,
           label + ": exactly one budget row exists");
    const auto budget = harness.budgets.snapshot(
        std::string{openai_budget_scope()}, harness.budget_now,
        openai_bootstrap_budget_policy());
    expect(budget.total_used == 1,
           label + ": exactly one durable provider permit is consumed");
    expect(harness.provider.calls == 1,
           label + ": fake provider was invoked exactly once");
}

void test_success_settles_and_rearms_once()
{
    TemporaryDatabases database("success");
    std::int64_t restart_ms = 0;
    std::int64_t settled_due_ms = 0;

    {
        Harness harness(database.state, database.sidecar, 1000000000);
        const auto prepared = harness.prepare_pulse("success");
        const auto gate = harness.gate.evaluate(prepared);
        expect(gate.result == GateResult::eligible && gate.task_id
                   && *gate.task_id == prepared.current_task_id,
               "success: provider gate names exact pulse-prepared Task");

        const auto before = harness.tasks.find(prepared.current_task_id);
        if (!before) throw std::runtime_error("success current Task missing");
        const auto exact_input = before->input;
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            decision_continue("Advance one bounded autonomous continuity objective."),
            {}, {}};

        expect(harness.executor.execute(
                   prepared.current_task_id,
                   "autonomous-provider-fake-worker",
                   harness.cognition_handler) == ExecuteResult::completed,
               "success: exact prepared Task completes through fake provider");
        const auto done = harness.tasks.find(prepared.current_task_id);
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->content_type == resume_after_wake_decision_content_type
                   && done->result->output.find("\"decision\":\"continue\"")
                        != std::string::npos,
               "success: normalized cognition result is durable");
        expect(harness.provider.last_request
                   && harness.provider.last_request->input == exact_input,
               "success: fake provider receives exact durable cognition bytes");
        expect_one_provider_effect(
            harness, database, prepared.current_task_id, "success");
        const auto action = harness.actions.find(action_id(prepared.current_task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "success: provider Action is confirmed exactly once");

        const auto after_execution = harness.gate.evaluate(prepared);
        expect(after_execution.result == GateResult::waiting
                   && !after_execution.task_id,
               "success: terminal cognition is never eligible for a second provider call");

        harness.advance(std::chrono::duration_cast<std::chrono::milliseconds>(1min));
        const auto settled = harness.pulse.observe();
        expect(settled.result == PulseResult::settled_continue && settled.cursor,
               "success: pulse settles canonical continue result");
        if (!settled.cursor) return;
        expect(settled.cursor->generation == prepared.generation + 1,
               "success: pulse generation advances exactly once");
        expect(settled.cursor->predecessor_task_id == prepared.current_task_id,
               "success: terminal cognition becomes exact next predecessor");
        expect(settled.cursor->state == AutonomousCognitionPulseState::idle,
               "success: continue settlement returns pulse to idle");
        expect(settled.cursor->due_at_ms - settled.cursor->anchor_at_ms
                   == std::chrono::duration_cast<std::chrono::milliseconds>(6h).count(),
               "success: pulse rearms exact six-hour cadence");
        settled_due_ms = settled.cursor->due_at_ms;
        restart_ms = milliseconds(harness.work_now);

        const auto once_more = harness.pulse.observe();
        expect(once_more.result == PulseResult::not_due,
               "success: immediate second pulse observation does not create another cycle");
        expect(row_count(database.state, "actions") == 1
                   && row_count(database.state, "budget_consumptions") == 1
                   && harness.provider.calls == 1,
               "success: second observation creates zero additional provider effects");
    }

    {
        Harness reopened(database.state, database.sidecar, restart_ms);
        const auto cursor = reopened.pulse_store.find(autonomous_cognition_pulse_scope);
        expect(cursor && cursor->generation == 1
                   && cursor->due_at_ms == settled_due_ms,
               "success restart: settled generation and deadline survive reopen exactly");
        expect(reopened.pulse.observe().result == PulseResult::not_due,
               "success restart: reopened pulse waits for durable deadline");
        expect(reopened.provider.calls == 0
                   && row_count(database.state, "actions") == 1
                   && row_count(database.state, "budget_consumptions") == 1,
               "success restart: reopen performs zero provider replay");
    }
}

void test_confirmed_effect_crash_window_never_replays()
{
    TemporaryDatabases database("confirmed-crash");
    std::int64_t restart_ms = 0;

    {
        Harness harness(database.state, database.sidecar, 2000000000);
        const auto prepared = harness.prepare_pulse("confirmed-crash");
        const auto gate = harness.gate.evaluate(prepared);
        expect(gate.result == GateResult::eligible,
               "confirmed crash: prepared Task is initially eligible");
        const auto task = harness.tasks.find(prepared.current_task_id);
        if (!task) throw std::runtime_error("confirmed crash current Task missing");

        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            decision_continue("This response is confirmed before the synthetic crash."),
            {}, {}};
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::succeeded,
               "confirmed crash: handler receives one definite fake provider response");
        expect(harness.tasks.find(prepared.current_task_id)
                   && harness.tasks.find(prepared.current_task_id)->status
                        == TaskStatus::pending,
               "confirmed crash: synthetic crash leaves Task result non-durable");
        expect_one_provider_effect(
            harness, database, prepared.current_task_id, "confirmed crash");
        const auto action = harness.actions.find(action_id(prepared.current_task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "confirmed crash: confirmed provider Action survives crash window");

        const auto blocked = harness.gate.evaluate(prepared);
        expect(blocked.result == GateResult::blocked
                   && blocked.detail.find("replay forbidden") != std::string::npos,
               "confirmed crash: gate blocks existing provider Action before replay");
        const auto second = harness.cognition_handler.execute(context);
        expect(second.outcome == HandlerOutcome::manual_review
                   && second.failure_code == "provider_response_not_durable"
                   && harness.provider.calls == 1,
               "confirmed crash: underlying provider boundary also refuses replay");
        restart_ms = milliseconds(harness.work_now);
    }

    {
        Harness reopened(database.state, database.sidecar, restart_ms);
        const auto cursor = reopened.pulse_store.find(autonomous_cognition_pulse_scope);
        if (!cursor) throw std::runtime_error("confirmed crash cursor missing after reopen");
        const auto gate = reopened.gate.evaluate(*cursor);
        expect(gate.result == GateResult::blocked
                   && reopened.provider.calls == 0,
               "confirmed crash restart: durable Action blocks provider after reopen");
        expect(row_count(database.state, "actions") == 1
                   && row_count(database.state, "budget_consumptions") == 1,
               "confirmed crash restart: durable effect counts remain exactly one");
    }
}

void test_malformed_result_blocks_after_one_effect()
{
    TemporaryDatabases database("malformed");
    std::int64_t restart_ms = 0;

    {
        Harness harness(database.state, database.sidecar, 3000000000);
        const auto prepared = harness.prepare_pulse("malformed");
        expect(harness.gate.evaluate(prepared).result == GateResult::eligible,
               "malformed: prepared Task is initially eligible");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            "not-json",
            {}, {}};

        expect(harness.executor.execute(
                   prepared.current_task_id,
                   "autonomous-provider-fake-worker",
                   harness.cognition_handler) == ExecuteResult::completed,
               "malformed: invalid provider output closes Task lifecycle");
        const auto done = harness.tasks.find(prepared.current_task_id);
        expect(done && done->status == TaskStatus::failed && done->result
                   && done->result->failure_code == "cognition_invalid_resume_decision",
               "malformed: invalid cognition result fails closed durably");
        expect_one_provider_effect(
            harness, database, prepared.current_task_id, "malformed");

        harness.advance(std::chrono::duration_cast<std::chrono::milliseconds>(1min));
        const auto blocked = harness.pulse.observe();
        expect(blocked.result == PulseResult::blocked && blocked.cursor
                   && blocked.cursor->state == AutonomousCognitionPulseState::blocked,
               "malformed: pulse blocks instead of scheduling another provider call");
        if (blocked.cursor) {
            expect(harness.gate.evaluate(*blocked.cursor).result == GateResult::blocked,
                   "malformed: blocked pulse cannot regain provider authority");
        }
        expect(harness.pulse.observe().result == PulseResult::blocked,
               "malformed: repeated observation remains blocked");
        expect(harness.provider.calls == 1
                   && row_count(database.state, "actions") == 1
                   && row_count(database.state, "budget_consumptions") == 1,
               "malformed: repeated observation never spends a second effect");
        restart_ms = milliseconds(harness.work_now);
    }

    {
        Harness reopened(database.state, database.sidecar, restart_ms);
        expect(reopened.pulse.observe().result == PulseResult::blocked,
               "malformed restart: blocked state survives reopen");
        expect(reopened.provider.calls == 0
                   && row_count(database.state, "actions") == 1
                   && row_count(database.state, "budget_consumptions") == 1,
               "malformed restart: reopen cannot replay provider effect");
    }
}

} // namespace

int main()
{
    test_success_settles_and_rearms_once();
    test_confirmed_effect_crash_window_never_replays();
    test_malformed_result_blocks_after_one_effect();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All autonomous pulse fake-provider integration tests passed\n";
    return 0;
}
