#include "AutonomousCognitionStaleRefresh.hpp"
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
using TaskStatus = gaudere::work::TaskStatus;
using RefreshResult = AutonomousCognitionStaleRefreshResult;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryFiles {
    explicit TemporaryFiles(std::string label)
    {
        const auto token = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto root = std::filesystem::temp_directory_path();
        state = root / ("gaudere-stale-refresh-state-" + label + "-" + token + ".db");
        pulse = root / ("gaudere-stale-refresh-pulse-" + label + "-" + token + ".db");
    }

    ~TemporaryFiles()
    {
        std::error_code ignored;
        for (const auto& path : {state, pulse}) {
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.string() + "-wal", ignored);
            std::filesystem::remove(path.string() + "-shm", ignored);
        }
    }

    std::filesystem::path state;
    std::filesystem::path pulse;
};

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
    task.id = "cognition.resume-after-wake.v0:stale-refresh-bootstrap";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "provider-free stale refresh bootstrap";
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
    std::string current_id;
};

struct Fixture {
    explicit Fixture(const std::string& label)
        : files(label),
          tasks(files.state.string()),
          actions(files.state.string()),
          budgets(files.state.string()),
          pulse_store(files.pulse.string()),
          now(gaudere::work::TimePoint{std::chrono::milliseconds{2000000000000LL}}),
          runtime(tasks, [this] { return now; }),
          gate(files.state.string(), tasks, budgets, actions,
               [this] { return now; }),
          refresh(pulse_store, tasks, actions, runtime, gate)
    {
        runtime.recover();
    }

    Prepared prepare()
    {
        const auto bootstrap = bootstrap_resume_task();
        tasks.save(bootstrap);

        ResumeContextSnapshotRecorder seed_recorder(
            tasks, runtime, [this] { return now; });
        const auto seed_snapshot = seed_recorder.record(snapshot_request(
            "Stale refresh predecessor context.", "stale-refresh-seed"));
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
            decision_continue("Let the pulse prepare another bounded cognition."),
            {}, {}};
        tasks.save(predecessor);

        now += 1min;
        ResumeContextSnapshotRecorder recorder(
            tasks, runtime, [this] { return now; });
        const auto snapshot = recorder.record(snapshot_request(
            "Current autonomous pulse facts.", "stale-refresh-current"));
        if (!snapshot.task)
            throw std::runtime_error("could not create current snapshot");
        const auto claim = cycle.claim(predecessor.id, snapshot.task->id);
        if (!claim.task)
            throw std::runtime_error("could not create current cognition claim");

        AutonomousCognitionPulseCursor cursor;
        cursor.revision = 7;
        cursor.generation = 4;
        cursor.state = AutonomousCognitionPulseState::prepared;
        cursor.predecessor_task_id = predecessor.id;
        cursor.predecessor_result_sha256 = sha256_hex(predecessor.result->output);
        cursor.anchor_at_ms = millis(now - 6h);
        cursor.due_at_ms = millis(now);
        cursor.observed_at_ms = millis(now);
        cursor.snapshot_task_id = snapshot.task->id;
        cursor.current_task_id = claim.task->id;
        if (!valid_autonomous_cognition_pulse_cursor(cursor))
            throw std::runtime_error("prepared pulse cursor is non-canonical");
        const auto seeded = pulse_store.seed(cursor);
        if (seeded.result != AutonomousCognitionPulseStoreResult::accepted)
            throw std::runtime_error("could not seed prepared pulse cursor");
        return {cursor, claim.task->id};
    }

    void make_stale()
    {
        now += current_cognition_max_snapshot_age + 1ms;
    }

    TemporaryFiles files;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    AutonomousCognitionPulseStore pulse_store;
    gaudere::work::TimePoint now;
    gaudere::work::Runtime runtime;
    AutonomousCognitionProviderGate gate;
    AutonomousCognitionStaleRefresh refresh;
};

void test_stale_unspent_task_retires_same_generation()
{
    Fixture fixture("retire");
    const auto prepared = fixture.prepare();
    fixture.make_stale();

    const auto before_budget = fixture.budgets.snapshot(
        std::string{openai_budget_scope()}, fixture.now,
        openai_bootstrap_budget_policy());
    const auto result = fixture.refresh.step();
    expect(result.result == RefreshResult::retired,
           "stale never-started cognition is retired");

    const auto task = fixture.tasks.find(prepared.current_id);
    expect(task && task->status == TaskStatus::cancelled,
           "retired cognition is durably cancelled");
    expect(task && task->attempts_started == 0,
           "retirement does not start a provider attempt");
    expect(task && task->cancel_reason == autonomous_cognition_stale_retirement_reason,
           "retirement uses exact durable cancellation marker");
    expect(task && task->result && task->result->failure_code == "cancelled"
               && task->result->failure_message
                    == autonomous_cognition_stale_retirement_reason,
           "runtime cancellation result carries exact retirement marker");

    const auto cursor = fixture.pulse_store.find(autonomous_cognition_pulse_scope);
    expect(cursor && cursor->state == AutonomousCognitionPulseState::idle,
           "retired cursor returns to due idle state");
    expect(cursor && cursor->generation == prepared.cursor.generation,
           "retirement does not increment cognition generation");
    expect(cursor && cursor->predecessor_task_id
                       == prepared.cursor.predecessor_task_id
               && cursor->predecessor_result_sha256
                       == prepared.cursor.predecessor_result_sha256,
           "retirement preserves predecessor evidence");
    expect(cursor && cursor->anchor_at_ms == prepared.cursor.anchor_at_ms
               && cursor->due_at_ms == prepared.cursor.due_at_ms,
           "retirement preserves anchor and already-due deadline");
    expect(cursor && !cursor->observed_at_ms
               && cursor->snapshot_task_id.empty()
               && cursor->current_task_id.empty(),
           "retirement clears only frozen preparation linkage");
    expect(!fixture.actions.find(std::string{openai_budget_scope()} + ":"
                                 + prepared.current_id),
           "retirement creates no provider Action");

    const auto after_budget = fixture.budgets.snapshot(
        std::string{openai_budget_scope()}, fixture.now,
        openai_bootstrap_budget_policy());
    expect(after_budget.total_used == before_budget.total_used
               && after_budget.in_window_used == before_budget.in_window_used,
           "retirement consumes no provider budget");
    expect(fixture.refresh.step().result == RefreshResult::not_applicable,
           "repeated retirement is idempotent after cursor reset");
}

void test_stale_task_with_attempt_evidence_blocks()
{
    Fixture fixture("attempt");
    const auto prepared = fixture.prepare();
    auto task = *fixture.tasks.find(prepared.current_id);
    task.attempts_started = 1;
    fixture.tasks.save(task);
    fixture.make_stale();

    const auto result = fixture.refresh.step();
    expect(result.result == RefreshResult::blocked,
           "stale cognition with attempt evidence fails closed");
    const auto stored = fixture.tasks.find(prepared.current_id);
    expect(stored && stored->status == TaskStatus::pending,
           "attempt evidence is never retired automatically");
}

void test_stale_task_with_action_evidence_blocks()
{
    Fixture fixture("action");
    const auto prepared = fixture.prepare();
    gaudere::scheduling::wake::Action action;
    action.id = std::string{openai_budget_scope()} + ":" + prepared.current_id;
    action.idempotency_key = action.id;
    action.critical = true;
    fixture.actions.save(action);
    fixture.make_stale();

    const auto result = fixture.refresh.step();
    expect(result.result == RefreshResult::blocked,
           "stale cognition with provider Action evidence fails closed");
    const auto stored = fixture.tasks.find(prepared.current_id);
    expect(stored && stored->status == TaskStatus::pending,
           "Action evidence forbids automatic cancellation/replay");
}

void test_crash_after_cancellation_recovers_cursor_only()
{
    Fixture fixture("crash");
    const auto prepared = fixture.prepare();
    fixture.make_stale();
    expect(fixture.runtime.request_cancel(
               prepared.current_id, autonomous_cognition_stale_retirement_reason),
           "fixture persists exact cancellation before simulated crash");

    AutonomousCognitionProviderGate restarted_gate(
        fixture.files.state.string(), fixture.tasks, fixture.budgets,
        fixture.actions, [&fixture] { return fixture.now; });
    AutonomousCognitionStaleRefresh restarted(
        fixture.pulse_store, fixture.tasks, fixture.actions,
        fixture.runtime, restarted_gate);
    const auto result = restarted.step();
    expect(result.result == RefreshResult::retired,
           "restart recognizes exact durable cancellation marker");
    const auto cursor = fixture.pulse_store.find(autonomous_cognition_pulse_scope);
    expect(cursor && cursor->state == AutonomousCognitionPulseState::idle
               && cursor->generation == prepared.cursor.generation,
           "crash recovery finishes same-generation cursor reset");
}

void test_other_terminal_marker_never_resets_pulse()
{
    Fixture fixture("foreign-cancel");
    const auto prepared = fixture.prepare();
    fixture.make_stale();
    expect(fixture.runtime.request_cancel(prepared.current_id, "operator request"),
           "fixture records unrelated cancellation");

    const auto result = fixture.refresh.step();
    expect(result.result == RefreshResult::blocked,
           "unrelated terminal cancellation cannot impersonate stale retirement");
    const auto cursor = fixture.pulse_store.find(autonomous_cognition_pulse_scope);
    expect(cursor && cursor->state == AutonomousCognitionPulseState::prepared,
           "foreign cancellation leaves prepared pulse fail-closed");
}

} // namespace

int main()
{
    test_stale_unspent_task_retires_same_generation();
    test_stale_task_with_attempt_evidence_blocks();
    test_stale_task_with_action_evidence_blocks();
    test_crash_after_cancellation_recovers_cursor_only();
    test_other_terminal_marker_never_resets_pulse();

    if (failures != 0) {
        std::cerr << failures << " autonomous stale-refresh checks failed\n";
        return 1;
    }
    std::cout << "autonomous_cognition_stale_refresh=PASS\n"
              << "provider_effects=0\n"
              << "generation_increment=0\n";
    return 0;
}
