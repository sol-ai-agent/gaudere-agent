#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "CanonicalCognitionDecision.hpp"
#include "ContinuityDeltaCheckpoint.hpp"
#include "CurrentCognitionCycle.hpp"
#include "CurrentCognitionHandler.hpp"
#include "OpenAIBudget.hpp"
#include "Provider.hpp"
#include "ProviderTaskHandler.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"
#include "TaskExecutor.hpp"

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
        state = root / ("gaudere-continuity-delta-state-" + label + "-"
                        + token + ".db");
        sidecar = root / ("gaudere-continuity-delta-pulse-" + label + "-"
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

std::string decision_continue(const std::string& objective)
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "continue"},
                {"reason", "Durable evidence supports one bounded continuity step."},
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
    task.id = "cognition.resume-after-wake.v0:delta-checkpoint-" + label;
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "continuity delta checkpoint bootstrap";
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
          executor(work_runtime, tasks)
    {
        work_runtime.recover();
        action_runtime.recover();
    }

    void set_now(const gaudere::work::TimePoint value)
    {
        work_now = value;
        const auto duration = value.time_since_epoch();
        action_now = gaudere::scheduling::wake::TimePoint{duration};
        budget_now = gaudere::budget::TimePoint{duration};
    }

    std::string make_seed_current(const std::string& label)
    {
        const auto bootstrap = bootstrap_resume_task(label);
        tasks.save(bootstrap);
        ResumeContextSnapshotRecorder recorder(
            tasks, work_runtime, [this] { return work_now; });
        const auto content = "Seed evidence for continuity delta " + label;
        const auto snapshot = recorder.record(snapshot_request(
            content, "continuity-delta-seed-" + label));
        if (!snapshot.task) throw std::runtime_error("could not create seed snapshot");
        CurrentCognitionCycle cycle(
            tasks, work_runtime, [this] { return work_now; }, true);
        const auto claim = cycle.claim(bootstrap.id, snapshot.task->id);
        if (!claim.task) throw std::runtime_error("could not create seed cognition");
        auto completed = *claim.task;
        completed.attempts_started = 1;
        completed.status = TaskStatus::succeeded;
        completed.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type,
            decision_continue("Start the provider-owned pulse lineage."), {}, {}};
        tasks.save(completed);
        if (!valid_current_cognition_task(completed))
            throw std::runtime_error("seed cognition is non-canonical");
        return completed.id;
    }

    std::string prepare_next(const std::string& predecessor)
    {
        const auto existing = pulse_store.find(autonomous_cognition_pulse_scope);
        if (!existing) {
            const auto seeded = pulse.seed(predecessor);
            if (seeded.result != PulseResult::seeded || !seeded.cursor)
                throw std::runtime_error("could not seed pulse");
            set_now(gaudere::work::TimePoint{
                std::chrono::milliseconds{seeded.cursor->due_at_ms}});
        } else {
            set_now(gaudere::work::TimePoint{
                std::chrono::milliseconds{existing->due_at_ms}});
        }
        const auto prepared = pulse.observe();
        if (prepared.result != PulseResult::prepared || !prepared.task)
            throw std::runtime_error("could not prepare pulse cognition: " + prepared.detail);
        return prepared.task->id;
    }

    std::string execute_current(const std::string& task_id,
                                const std::string& objective)
    {
        provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            resume_after_wake_decision_content_type,
            decision_continue(objective), {}, {}};
        if (executor.execute(task_id, "continuity-delta-provider", cognition_handler)
            != ExecuteResult::completed) {
            throw std::runtime_error("fake provider cognition did not complete");
        }
        const auto task = tasks.find(task_id);
        if (!task || task->status != TaskStatus::succeeded)
            throw std::runtime_error("fake provider cognition is not succeeded");
        return task_id;
    }

    void settle_pulse()
    {
        const auto settled = pulse.observe();
        if (settled.result != PulseResult::settled_continue)
            throw std::runtime_error("pulse did not settle continued cognition");
    }

    std::string make_two_provider_calls(const std::string& label)
    {
        const auto seed = make_seed_current(label);
        const auto first = prepare_next(seed);
        execute_current(first, "Produce the next bounded continuity delta.");
        settle_pulse();
        const auto second = prepare_next(first);
        execute_current(second, "Reconcile the prior provider delta provider-free.");
        return second;
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
};

void canonical_checkpoint_and_repeats()
{
    TemporaryDatabases files("canonical");
    Harness harness(files.state, files.sidecar, 1'800'000'000'000LL);
    const auto audited = harness.make_two_provider_calls("canonical");
    expect(harness.provider.calls == 2, "fixture must contain exactly two provider calls");
    const auto budget_before = harness.budgets.snapshot(
        std::string{openai_budget_scope()}, harness.budget_now,
        openai_bootstrap_budget_policy());
    const auto actions_before = row_count(files.state, "actions");
    const auto wake_before = row_count(files.state, "wake_intents");
    const auto tasks_before = row_count(files.state, "tasks");

    ContinuityDeltaCheckpoint checkpoint(
        harness.tasks, harness.actions, harness.budgets, harness.wakes,
        harness.work_runtime, [&harness] { return harness.work_now; });
    const auto created = checkpoint.checkpoint(audited);
    expect(created.result == ContinuityDeltaCheckpointResult::accepted,
           "canonical lineage should create one checkpoint");
    expect(created.task && created.task->status == TaskStatus::succeeded,
           "checkpoint should finish as succeeded local Task");
    expect(row_count(files.state, "tasks") == tasks_before + 1,
           "checkpoint should add exactly one Task row");
    expect(row_count(files.state, "actions") == actions_before,
           "checkpoint must add no Action rows");
    expect(row_count(files.state, "wake_intents") == wake_before,
           "checkpoint must not change WakeIntent rows");
    const auto budget_after = harness.budgets.snapshot(
        std::string{openai_budget_scope()}, harness.budget_now,
        openai_bootstrap_budget_policy());
    expect(budget_after.total_used == budget_before.total_used,
           "checkpoint must not consume provider budget");

    if (created.task) {
        const auto payload = Json::parse(created.task->input);
        expect(payload.at("schema") == continuity_delta_checkpoint_schema,
               "checkpoint schema should be canonical v1");
        expect(payload.at("audited").at("task_id") == audited,
               "checkpoint should bind the audited cognition");
        expect(payload.at("audited_context").at("provider_total_before") == 1,
               "second cognition context should observe one prior provider effect");
        expect(payload.at("current_provider_budget").at("total_used") == 2,
               "checkpoint should observe two durable provider effects");
        expect(payload.at("predecessor").at("provider_action").at("effect")
                   == "confirmed",
               "predecessor provider Action should be confirmed");
        expect(payload.at("audited").at("provider_action").at("effect")
                   == "confirmed",
               "audited provider Action should be confirmed");
        expect(payload.at("unresolved_external").size() == 3,
               "host-only provenance should remain explicit unresolved evidence");
        expect(created.task->result
                   && created.task->result->output == created.task->input,
               "checkpoint result should echo exact canonical bytes");
    }

    const auto checkpoint_id = created.task ? created.task->id : std::string{};
    for (int repeat = 0; repeat < 100; ++repeat) {
        const auto duplicate = checkpoint.checkpoint(audited);
        expect(duplicate.result == ContinuityDeltaCheckpointResult::duplicate,
               "repeat checkpoint should be durable duplicate");
        expect(duplicate.task && duplicate.task->id == checkpoint_id,
               "repeat checkpoint should preserve exact identity");
    }
    expect(row_count(files.state, "tasks") == tasks_before + 1,
           "100 repeats must not create extra checkpoint Tasks");
    expect(harness.provider.calls == 2,
           "checkpoint repeats must never invoke provider");
}

void crash_after_submit_recovers_same_checkpoint()
{
    TemporaryDatabases files("crash");
    Harness harness(files.state, files.sidecar, 1'800'100'000'000LL);
    const auto audited = harness.make_two_provider_calls("crash");
    bool injected = false;
    ContinuityDeltaCheckpoint crashing(
        harness.tasks, harness.actions, harness.budgets, harness.wakes,
        harness.work_runtime, [&harness] { return harness.work_now; },
        [&injected](const std::string_view phase) {
            if (!injected && phase == "after_submit") {
                injected = true;
                throw std::runtime_error("simulated checkpoint crash");
            }
        });
    const auto interrupted = crashing.checkpoint(audited);
    expect(interrupted.result == ContinuityDeltaCheckpointResult::unavailable,
           "simulated post-submit crash should surface unavailable");
    const auto pending = harness.tasks.find_pending_for(
        {continuity_delta_checkpoint_task_kind});
    expect(pending.has_value(), "post-submit crash should leave one durable pending checkpoint");
    const auto pending_id = pending ? pending->id : std::string{};

    harness.work_runtime.recover();
    ContinuityDeltaCheckpoint reopened(
        harness.tasks, harness.actions, harness.budgets, harness.wakes,
        harness.work_runtime, [&harness] { return harness.work_now; });
    const auto recovered = reopened.checkpoint(audited);
    expect(recovered.result == ContinuityDeltaCheckpointResult::duplicate,
           "reopen should complete the same durable checkpoint");
    expect(recovered.task && recovered.task->id == pending_id
               && recovered.task->status == TaskStatus::succeeded,
           "reopen should preserve checkpoint identity and complete it");
    expect(harness.provider.calls == 2,
           "checkpoint crash recovery must invoke no provider");
}

void missing_provider_action_fails_closed()
{
    TemporaryDatabases files("missing-action");
    Harness harness(files.state, files.sidecar, 1'800'200'000'000LL);
    const auto seed = harness.make_seed_current("missing-action");
    const auto audited = harness.prepare_next(seed);
    auto task = harness.tasks.find(audited);
    if (!task) throw std::runtime_error("missing-action fixture task absent");
    task->attempts_started = 1;
    task->status = TaskStatus::succeeded;
    task->result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("No provider Action exists for this synthetic completion."),
        {}, {}};
    harness.tasks.save(*task);

    ContinuityDeltaCheckpoint checkpoint(
        harness.tasks, harness.actions, harness.budgets, harness.wakes,
        harness.work_runtime, [&harness] { return harness.work_now; });
    const auto result = checkpoint.checkpoint(audited);
    expect(result.result == ContinuityDeltaCheckpointResult::ineligible,
           "missing provider Action should fail closed");
    expect(harness.provider.calls == 0,
           "missing-action failure must not invoke provider");
}

void later_budget_drift_conflicts()
{
    TemporaryDatabases files("budget-drift");
    Harness harness(files.state, files.sidecar, 1'800'300'000'000LL);
    const auto audited = harness.make_two_provider_calls("budget-drift");
    harness.set_now(harness.work_now + 20min);
    const auto consumed = harness.budgets.consume(
        std::string{openai_budget_scope()}, "continuity-delta-unrelated-third-call",
        harness.budget_now, openai_bootstrap_budget_policy());
    expect(consumed == gaudere::budget::ConsumeResult::accepted,
           "fixture should create one later budget delta");

    ContinuityDeltaCheckpoint checkpoint(
        harness.tasks, harness.actions, harness.budgets, harness.wakes,
        harness.work_runtime, [&harness] { return harness.work_now; });
    const auto result = checkpoint.checkpoint(audited);
    expect(result.result == ContinuityDeltaCheckpointResult::conflict,
           "later provider budget drift should prevent retroactive checkpoint rewrite");
    expect(!harness.tasks.find_pending_for({continuity_delta_checkpoint_task_kind}),
           "budget conflict should create no checkpoint Task");
}

} // namespace

int main()
{
    canonical_checkpoint_and_repeats();
    crash_after_submit_recovers_same_checkpoint();
    missing_provider_action_fails_closed();
    later_budget_drift_conflicts();

    if (failures != 0) {
        std::cerr << failures << " continuity delta checkpoint assertion(s) failed\n";
        return 1;
    }
    std::cout << "continuity delta checkpoint provider-free proof PASS\n";
    return 0;
}
