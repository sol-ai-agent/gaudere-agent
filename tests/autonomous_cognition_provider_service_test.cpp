#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionProviderService.hpp"
#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseService.hpp"
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
#include <gaudere/scheduling/wake/Scheduler.hpp>
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
        state = root / ("gaudere-autonomous-provider-service-state-" + label
                        + "-" + token + ".db");
        sidecar = root / ("gaudere-autonomous-provider-service-pulse-" + label
                          + "-" + token + ".db");
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
                {"reason", "Fresh durable evidence supports one bounded step."},
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
    task.id = "cognition.resume-after-wake.v0:provider-service-" + label;
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "autonomous provider service bootstrap";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("Create one current cognition predecessor."), {}, {}};
    return task;
}

class FakeProvider final : public Provider {
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

struct Fixture {
    Fixture(const std::filesystem::path& state,
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
          pulse_service(pulse, budgets, scheduler, [this] { return work_now; }),
          provider_handler(action_runtime, actions, provider, budgets,
                           openai_bootstrap_budget_policy(),
                           [this] { return budget_now; }),
          cognition_handler(provider_handler),
          executor(work_runtime, tasks),
          gate(state_path, tasks, budgets, actions, [this] { return work_now; }),
          provider_service(pulse_service, gate, executor, cognition_handler,
                           tasks, scheduler, [this] { return work_now; })
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
        const auto content = "Provider service seed evidence " + label;
        const auto snapshot = recorder.record(snapshot_request(
            content, "provider-service-seed-" + label));
        if (!snapshot.task)
            throw std::runtime_error("could not create provider service snapshot");
        CurrentCognitionCycle cycle(
            tasks, work_runtime, [this] { return work_now; }, true);
        const auto claim = cycle.claim(bootstrap.id, snapshot.task->id);
        if (!claim.task)
            throw std::runtime_error("could not create provider service predecessor");
        auto completed = *claim.task;
        completed.attempts_started = 1;
        completed.status = TaskStatus::succeeded;
        completed.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type,
            decision_continue("Let the pulse prepare the next bounded cognition."), {}, {}};
        tasks.save(completed);
        return completed.id;
    }

    AutonomousCognitionPulseCursor prepare(const std::string& label)
    {
        const auto predecessor = make_seed_current(label);
        const auto seeded = pulse.seed(predecessor);
        if (!seeded.cursor)
            throw std::runtime_error("could not seed provider service pulse");
        set_now(gaudere::work::TimePoint{
            std::chrono::milliseconds{seeded.cursor->due_at_ms}});
        const auto prepared = pulse.observe();
        if (prepared.result != PulseResult::prepared || !prepared.cursor)
            throw std::runtime_error("could not prepare provider service pulse");
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
    gaudere::scheduling::wake::Scheduler scheduler;
    AutonomousCognitionPulse pulse;
    AutonomousCognitionPulseService pulse_service;
    FakeProvider provider;
    ProviderTaskHandler provider_handler;
    CurrentCognitionHandler cognition_handler;
    TaskExecutor executor;
    AutonomousCognitionProviderGate gate;
    AutonomousCognitionProviderService provider_service;
};

void test_service_executes_and_settles_one_cycle()
{
    TemporaryDatabases database("success");
    Fixture fixture(database.state, database.sidecar, 1000000000);
    const auto prepared = fixture.prepare("success");
    const auto before = fixture.tasks.find(prepared.current_task_id);
    if (!before) throw std::runtime_error("prepared Task missing");
    const auto exact_input = before->input;
    fixture.provider.next_result = ProviderResult{
        ProviderOutcome::succeeded,
        "text/plain",
        decision_continue("Continue with one newly chosen bounded objective."), {}, {}};

    const auto step = fixture.provider_service.step();
    expect(step.healthy, "success: service remains healthy");
    expect(step.provider_executed, "success: service crosses provider boundary once");
    expect(step.task_id && *step.task_id == prepared.current_task_id,
           "success: service executes exact pulse Task");
    expect(step.monitoring && step.next_at,
           "success: settled continue re-arms monitoring");
    expect(fixture.provider.calls == 1,
           "success: fake provider invoked exactly once");
    expect(fixture.provider.last_request
               && fixture.provider.last_request->input == exact_input,
           "success: provider receives exact durable Task input");
    expect(row_count(database.state, "actions") == 1,
           "success: exactly one provider Action is durable");
    expect(row_count(database.state, "budget_consumptions") == 1,
           "success: exactly one provider permit is durable");

    const auto cursor = fixture.pulse_store.find(autonomous_cognition_pulse_scope);
    expect(cursor && cursor->state == AutonomousCognitionPulseState::idle,
           "success: service settles pulse back to idle");
    expect(cursor && cursor->generation == prepared.generation + 1,
           "success: pulse generation advances once");
    expect(cursor && cursor->predecessor_task_id == prepared.current_task_id,
           "success: completed cognition becomes next predecessor");
    expect(cursor && cursor->due_at_ms - cursor->anchor_at_ms
               == std::chrono::duration_cast<std::chrono::milliseconds>(6h).count(),
           "success: next cognition remains on six-hour cadence");

    const auto again = fixture.provider_service.step();
    expect(!again.provider_executed && fixture.provider.calls == 1,
           "success: immediate re-observation cannot replay provider");
    expect(row_count(database.state, "actions") == 1
               && row_count(database.state, "budget_consumptions") == 1,
           "success: immediate re-observation adds zero effects");
}

void test_cooldown_schedules_retry_without_cursor_mutation()
{
    TemporaryDatabases database("cooldown");
    Fixture fixture(database.state, database.sidecar, 2000000000);
    const auto prepared = fixture.prepare("cooldown");
    expect(fixture.budgets.consume(
               std::string{openai_budget_scope()}, "cooldown-fixture",
               fixture.budget_now - 5min, openai_bootstrap_budget_policy())
               == gaudere::budget::ConsumeResult::accepted,
           "cooldown: fixture budget consumption accepted");

    const auto step = fixture.provider_service.step();
    expect(step.healthy && step.monitoring && step.next_at,
           "cooldown: service schedules one provider retry");
    expect(step.next_at && *step.next_at == fixture.work_now + 10min,
           "cooldown: retry is exact minimum-interval boundary");
    expect(fixture.scheduler.next() == step.next_at,
           "cooldown: shared scheduler owns retry deadline");
    expect(!step.provider_executed && fixture.provider.calls == 0,
           "cooldown: no provider effect is attempted");
    const auto cursor = fixture.pulse_store.find(autonomous_cognition_pulse_scope);
    expect(cursor && cursor->state == AutonomousCognitionPulseState::prepared
               && cursor->current_task_id == prepared.current_task_id,
           "cooldown: pulse cursor remains exactly prepared");
}

void test_existing_action_blocks_without_hot_loop()
{
    TemporaryDatabases database("existing-action");
    Fixture fixture(database.state, database.sidecar, 3000000000);
    const auto prepared = fixture.prepare("existing-action");
    gaudere::scheduling::wake::Action action;
    action.id = std::string{openai_budget_scope()} + ":" + prepared.current_task_id;
    action.idempotency_key = action.id;
    action.critical = true;
    fixture.actions.save(action);

    const auto step = fixture.provider_service.step();
    expect(step.healthy && !step.monitoring,
           "existing Action: service stays alive but disables provider monitoring");
    expect(!step.provider_executed && fixture.provider.calls == 0,
           "existing Action: provider is never replayed");
    expect(!step.next_at,
           "existing Action: blocked authority does not schedule a hot loop");
    expect(row_count(database.state, "actions") == 1
               && row_count(database.state, "budget_consumptions") == 0,
           "existing Action: blocked gate creates zero new effects");
}

} // namespace

int main()
{
    test_service_executes_and_settles_one_cycle();
    test_cooldown_schedules_retry_without_cursor_mutation();
    test_existing_action_blocks_without_hot_loop();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All autonomous provider service tests passed\n";
    return 0;
}
