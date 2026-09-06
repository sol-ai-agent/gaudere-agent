#include "ContinuityDeltaCheckpoint.hpp"
#include "LocalActivityPulse.hpp"
#include "LocalActivityPulseStore.hpp"
#include "LocalContinuityObservation.hpp"
#include "OpenAIBudget.hpp"
#include "ResumeAfterWake.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Action.hpp>
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
#include <string_view>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;
using namespace std::chrono_literals;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using Action = gaudere::scheduling::wake::Action;
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

std::string repeated(const char value)
{
    return std::string(64, value);
}

std::string cognition(const char value)
{
    return "cognition.current.v0:" + repeated(value);
}

std::string provider_action_id(const std::string& task_id)
{
    return "provider.call:openai.responses:" + task_id;
}

std::string empty_wake_canonical()
{
    return Json{{"scope", "cognition.reflect.wake.v0"},
                {"cardinality", "empty"}}.dump();
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

gaudere::work::TimePoint time_point(const std::int64_t value)
{
    return gaudere::work::TimePoint{std::chrono::milliseconds{value}};
}

struct TemporaryFiles {
    explicit TemporaryFiles(std::string label)
    {
        const auto token = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto root = std::filesystem::temp_directory_path();
        state = root / ("gaudere-local-activity-state-" + label + "-"
                        + token + ".db");
        sidecar = root / ("gaudere-local-activity-sidecar-" + label + "-"
                          + token + ".db");
    }

    ~TemporaryFiles()
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
        std::filesystem::remove(path.string() + "-journal", ignored);
        std::filesystem::remove(path.string() + ".lock", ignored);
    }

    std::filesystem::path state;
    std::filesystem::path sidecar;
};

std::int64_t scalar_sql(const std::filesystem::path& path,
                        const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database)
                                             : "sqlite open failed";
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        const std::string message = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    const int step = sqlite3_step(statement);
    const auto value = step == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (value < 0) throw std::runtime_error("sqlite scalar query failed");
    return value;
}

std::int64_t table_count(const std::filesystem::path& path, const char* table)
{
    return scalar_sql(path, "SELECT COUNT(*) FROM " + std::string{table});
}

std::int64_t local_observation_count(const std::filesystem::path& path)
{
    return scalar_sql(path,
        "SELECT COUNT(*) FROM tasks WHERE kind='continuity.local-observation.v1'");
}

Json canonical_decision(const std::string& reason)
{
    return Json{{"schema", resume_after_wake_decision_schema},
                {"decision", "stop"}, {"reason", reason}};
}

Json canonical_action_json(const std::string& task_id)
{
    const auto id = provider_action_id(task_id);
    return Json{{"id", id},
                {"idempotency_key", id},
                {"status", "succeeded"},
                {"effect", "confirmed"},
                {"critical", true}};
}

Task checkpoint_task(const char audited_hex,
                     const char predecessor_hex,
                     const char snapshot_hex)
{
    const auto audited = cognition(audited_hex);
    const auto predecessor = cognition(predecessor_hex);
    const Json payload = {
        {"schema", continuity_delta_checkpoint_schema},
        {"audited", Json{
            {"task_id", audited},
            {"result_sha256", repeated('c')},
            {"decision", canonical_decision("Synthetic audited checkpoint evidence.")},
            {"provider_action", canonical_action_json(audited)}
        }},
        {"predecessor", Json{
            {"task_id", predecessor},
            {"result_sha256", repeated('d')},
            {"decision", canonical_decision("Synthetic predecessor checkpoint evidence.")},
            {"provider_action", canonical_action_json(predecessor)}
        }},
        {"audited_context", Json{
            {"snapshot_task_id",
             "continuity.resume-context-snapshot.v1:" + repeated(snapshot_hex)},
            {"captured_at_ms", 1'700'000'000'000LL},
            {"provider_budget_scope", "provider.call:openai.responses"},
            {"provider_total_before", 9},
            {"historical_wake", Json{
                {"scope", "cognition.reflect.wake.v0"},
                {"cardinality", "empty"}
            }}
        }},
        {"current_provider_budget", Json{
            {"scope", "provider.call:openai.responses"},
            {"total_used", 10}
        }},
        {"reconciliation", Json{
            {"provider_increment_from_audited_context", 1},
            {"predecessor_provider_effect_confirmed", true},
            {"audited_provider_effect_confirmed", true},
            {"statement",
             "The durable audited context already includes the confirmed predecessor provider effect; older provider totals that omit that effect remain historical evidence and are superseded for current accounting."}
        }},
        {"unresolved_external", Json::array({
            "external_checkpoint_identity",
            "rollback_reference",
            "stopped_state_backup_marker"
        })}
    };

    Task task;
    task.input = payload.dump();
    task.id = std::string{continuity_delta_checkpoint_task_prefix}
        + sha256_hex(task.input);
    task.idempotency_key = std::string{continuity_delta_checkpoint_task_prefix}
        + "audited:" + audited;
    task.kind = continuity_delta_checkpoint_task_kind;
    task.input_content_type = continuity_delta_checkpoint_content_type;
    task.limits.max_input_bytes = 32 * 1024;
    task.limits.max_output_bytes = 32 * 1024;
    task.limits.max_runtime = 2s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        continuity_delta_checkpoint_content_type, task.input, {}, {}};
    return task;
}

Task cancelled_successor()
{
    Task task;
    task.id = cognition('f');
    task.idempotency_key = task.id;
    task.kind = "cognition.current.v0";
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "synthetic cancelled successor; must remain untouched";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 0;
    task.status = TaskStatus::cancelled;
    task.cancel_reason = "synthetic stale successor retired before effect";
    task.result = gaudere::work::TaskResult{
        "text/plain", {}, "cancelled",
        "synthetic stale successor retired before effect"};
    return task;
}

bool same_successor_state(const Task& before, const Task& after)
{
    return before.id == after.id
        && before.idempotency_key == after.idempotency_key
        && before.kind == after.kind
        && before.input == after.input
        && before.attempts_started == after.attempts_started
        && before.status == after.status
        && before.cancel_reason == after.cancel_reason
        && before.result.has_value() == after.result.has_value()
        && (!before.result
            || (before.result->content_type == after.result->content_type
                && before.result->output == after.result->output
                && before.result->failure_code == after.result->failure_code
                && before.result->failure_message == after.result->failure_message));
}

struct Fixture {
    explicit Fixture(const std::string& label,
                     const std::int64_t base_ms = 1'800'000'000'000LL)
        : files(label), state_path(files.state.string()),
          tasks(state_path), actions(state_path), budgets(state_path), wakes(state_path),
          now(time_point(base_ms))
    {
        install_budget(base_ms);
        anchor = checkpoint_task('a', 'b', 'e');
        tasks.save(anchor);
        install_action(cognition('a'));
        install_action(cognition('b'));
        successor = cancelled_successor();
        tasks.save(successor);
    }

    void install_budget(const std::int64_t base_ms)
    {
        const auto policy = openai_bootstrap_budget_policy();
        const auto step_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            25h).count();
        const auto start_ms = base_ms - 9 * step_ms;
        for (int index = 0; index < 10; ++index) {
            const auto result = budgets.consume(
                std::string{openai_budget_scope()},
                "synthetic-provider-permit-" + std::to_string(index),
                gaudere::budget::TimePoint{
                    std::chrono::milliseconds{start_ms + index * step_ms}},
                policy);
            if (result != gaudere::budget::ConsumeResult::accepted)
                throw std::runtime_error("could not install synthetic provider budget history");
        }
    }

    void install_action(const std::string& task_id)
    {
        Action action;
        action.id = provider_action_id(task_id);
        action.idempotency_key = action.id;
        action.critical = true;
        action.status = ActionStatus::succeeded;
        action.effect_result = EffectResult::confirmed;
        actions.save(action);
    }

    [[nodiscard]] std::uint64_t provider_total() const
    {
        return budgets.snapshot(
            std::string{openai_budget_scope()},
            gaudere::budget::TimePoint{now.time_since_epoch()},
            openai_bootstrap_budget_policy()).total_used;
    }

    TemporaryFiles files;
    std::string state_path;
    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint now;
    Task anchor;
    Task successor;
};

LocalActivityPulseObservation observe_with_fresh_runtime(
    Fixture& fixture,
    LocalActivityPulseStore& store,
    const bool enabled = true,
    LocalActivityPulse::PhaseHook hook = {})
{
    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, enabled, std::move(hook));
    return pulse.observe();
}

void disabled_and_unseeded_are_inert()
{
    Fixture fixture("disabled");
    const auto tasks_before = table_count(fixture.files.state, "tasks");
    LocalActivityPulseStore store(fixture.files.sidecar.string());

    {
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse disabled(
            store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; });
        expect(disabled.seed(fixture.anchor.id).result == PulseResult::disabled,
               "default-disabled local pulse refuses seed");
        expect(disabled.observe().result == PulseResult::disabled,
               "default-disabled local pulse refuses observation");
    }
    expect(!store.find(local_activity_pulse_scope),
           "disabled local pulse creates no durable cursor");
    expect(table_count(fixture.files.state, "tasks") == tasks_before,
           "disabled local pulse creates no Task");

    {
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse enabled(
            store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; }, true);
        expect(enabled.observe().result == PulseResult::unseeded,
               "enabled-but-unseeded local pulse remains inert");
    }
    expect(!store.find(local_activity_pulse_scope)
               && local_observation_count(fixture.files.state) == 0,
           "unseeded local pulse creates no cursor or observation Task");
}

void seed_and_due_observation()
{
    Fixture fixture("due");
    LocalActivityPulseStore store(fixture.files.sidecar.string());
    const auto actions_before = table_count(fixture.files.state, "actions");
    const auto wakes_before = table_count(fixture.files.state, "wake_intents");
    const auto successor_before = *fixture.tasks.find(fixture.successor.id);

    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, true);

    expect(pulse.seed("missing-checkpoint").result == PulseResult::ineligible,
           "unknown checkpoint cannot seed local pulse");
    const auto seeded = pulse.seed(fixture.anchor.id);
    expect(seeded.result == PulseResult::seeded && seeded.cursor,
           "exact succeeded checkpoint seeds local pulse once");
    if (!seeded.cursor) return;
    const auto seed_anchor_ms = seeded.cursor->anchor_at_ms;
    expect(seeded.cursor->due_at_ms
               == seed_anchor_ms + local_activity_pulse_cadence_ms,
           "first local observation is due exactly 24h after seed");
    expect(pulse.seed(fixture.anchor.id).result == PulseResult::duplicate,
           "same exact checkpoint seed is idempotent");

    const auto other = checkpoint_task('4', '5', '6');
    fixture.tasks.save(other);
    expect(pulse.seed(other.id).result == PulseResult::conflict,
           "different canonical checkpoint cannot rebind seeded cursor");

    fixture.now = time_point(seeded.cursor->due_at_ms - 1);
    expect(pulse.observe().result == PulseResult::not_due,
           "one millisecond before durable due creates no observation");
    expect(local_observation_count(fixture.files.state) == 0,
           "before due there is no local observation Task");

    fixture.now = time_point(seeded.cursor->due_at_ms);
    const auto observed = pulse.observe();
    expect(observed.result == PulseResult::settled && observed.cursor && observed.task,
           "at due exactly one local observation executes and settles");
    if (observed.cursor && observed.task) {
        expect(observed.cursor->generation == 1
                   && observed.cursor->state == LocalActivityPulseState::settled,
               "first generation settles durably");
        const auto inspection = inspect_local_continuity_observation_task(*observed.task);
        expect(inspection.eligible
                   && inspection.facts.generation == 1
                   && inspection.facts.provider_total == 10
                   && inspection.facts.anchor_checkpoint_task_id == fixture.anchor.id
                   && inspection.facts.anchor_checkpoint_result_sha256
                        == sha256_hex(fixture.anchor.input)
                   && inspection.facts.historical_wake_sha256
                        == sha256_hex(empty_wake_canonical()),
               "settled observation contains exact bounded anchor evidence");
        expect(observed.cursor->result_sha256
                   == std::optional<std::string>{sha256_hex(observed.task->input)},
               "cursor binds exact successful observation result hash");
    }
    expect(local_observation_count(fixture.files.state) == 1,
           "due admission creates exactly one semantic observation Task");
    expect(pulse.observe().result == PulseResult::not_due,
           "repeated observe after settlement does not duplicate generation");
    expect(local_observation_count(fixture.files.state) == 1,
           "repeated observe leaves exactly one observation Task");

    expect(fixture.provider_total() == 10,
           "local observation does not consume provider budget");
    expect(table_count(fixture.files.state, "actions") == actions_before,
           "local observation creates no Action rows");
    expect(table_count(fixture.files.state, "wake_intents") == wakes_before,
           "local observation creates or mutates no WakeIntent row");
    const auto successor_after = fixture.tasks.find(fixture.successor.id);
    expect(successor_after && same_successor_state(successor_before, *successor_after),
           "cancelled zero-attempt successor remains byte/semantic unchanged");
}

void crash_recovery_one_phase(const std::string& phase)
{
    Fixture fixture("crash-" + phase);
    std::int64_t due_at_ms = 0;
    bool injected = false;

    {
        LocalActivityPulseStore store(fixture.files.sidecar.string());
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse pulse(
            store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; }, true,
            [&phase, &injected](const std::string_view current) {
                if (!injected && current == phase) {
                    injected = true;
                    throw std::runtime_error("synthetic local pulse crash at " + phase);
                }
            });
        const auto seeded = pulse.seed(fixture.anchor.id);
        expect(seeded.result == PulseResult::seeded && seeded.cursor,
               phase + ": crash fixture seeds once");
        if (!seeded.cursor) return;
        due_at_ms = seeded.cursor->due_at_ms;
        fixture.now = time_point(due_at_ms);
        const auto interrupted = pulse.observe();
        expect(injected && interrupted.result == PulseResult::unavailable,
               phase + ": injected crash surfaces unavailable after durable phase");
    }

    {
        LocalActivityPulseStore reopened(fixture.files.sidecar.string());
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse recovered(
            reopened, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; }, true);
        const auto result = recovered.observe();
        if (phase == "after_settlement") {
            expect(result.result == PulseResult::not_due,
                   phase + ": restart observes already-settled generation without replay");
        } else {
            expect(result.result == PulseResult::settled && result.cursor,
                   phase + ": restart converges to one settled generation");
        }
        const auto cursor = reopened.find(local_activity_pulse_scope);
        expect(cursor && cursor->generation == 1
                   && cursor->state == LocalActivityPulseState::settled,
               phase + ": durable cursor converges to settled generation 1");
    }
    expect(local_observation_count(fixture.files.state) == 1,
           phase + ": crash recovery creates exactly one semantic Task");
    expect(fixture.provider_total() == 10,
           phase + ": crash recovery consumes no provider budget");
}

void crash_recovery_matrix()
{
    crash_recovery_one_phase("after_preparing");
    crash_recovery_one_phase("after_submit");
    crash_recovery_one_phase("after_execute");
    crash_recovery_one_phase("after_settlement");
}

void five_day_downtime_coalesces()
{
    Fixture fixture("downtime");
    LocalActivityPulseStore store(fixture.files.sidecar.string());
    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, true);

    const auto seeded = pulse.seed(fixture.anchor.id);
    expect(seeded.cursor.has_value(), "downtime fixture seeds");
    if (!seeded.cursor) return;
    const auto five_days_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        5 * 24h).count();
    fixture.now = time_point(seeded.cursor->due_at_ms + five_days_ms);
    const auto observed = pulse.observe();
    expect(observed.result == PulseResult::settled && observed.task && observed.cursor,
           "five-day downtime coalesces to one recovered observation");
    if (observed.task && observed.cursor) {
        const auto inspected = inspect_local_continuity_observation_task(*observed.task);
        expect(inspected.eligible
                   && inspected.facts.captured_at_ms - inspected.facts.due_at_ms
                        == five_days_ms,
               "recovered observation records exact five-day lateness");
        const auto captured = *observed.cursor->captured_at_ms;
        expect(pulse.observe().result == PulseResult::not_due,
               "no catch-up storm occurs immediately after late settlement");
        fixture.now = time_point(captured + local_activity_pulse_cadence_ms - 1);
        expect(pulse.observe().result == PulseResult::not_due,
               "next generation waits 24h after recovered capture");
    }
    expect(local_observation_count(fixture.files.state) == 1,
           "five missed days create one local Task, not five");
}

void clock_rollback_fails_closed()
{
    Fixture fixture("rollback");
    LocalActivityPulseStore store(fixture.files.sidecar.string());
    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, true);

    const auto seeded = pulse.seed(fixture.anchor.id);
    expect(seeded.cursor.has_value(), "rollback fixture seeds");
    if (!seeded.cursor) return;
    const auto seed_revision = seeded.cursor->revision;
    fixture.now = time_point(seeded.cursor->anchor_at_ms - 1);
    expect(pulse.observe().result == PulseResult::clock_rollback,
           "clock before seed anchor fails closed");
    auto cursor = store.find(local_activity_pulse_scope);
    expect(cursor && cursor->revision == seed_revision
               && local_observation_count(fixture.files.state) == 0,
           "pre-anchor rollback performs no durable mutation");

    fixture.now = time_point(seeded.cursor->due_at_ms);
    const auto settled = pulse.observe();
    expect(settled.result == PulseResult::settled && settled.cursor,
           "rollback fixture settles generation before second rollback test");
    if (!settled.cursor || !settled.cursor->captured_at_ms) return;
    const auto settled_revision = settled.cursor->revision;
    fixture.now = time_point(*settled.cursor->captured_at_ms - 1);
    expect(pulse.observe().result == PulseResult::clock_rollback,
           "clock before last capture fails closed");
    cursor = store.find(local_activity_pulse_scope);
    expect(cursor && cursor->revision == settled_revision
               && local_observation_count(fixture.files.state) == 1,
           "post-settlement rollback neither rewrites nor duplicates work");
}

void three_generations_then_quiescent()
{
    Fixture fixture("three-generations");
    LocalActivityPulseStore store(fixture.files.sidecar.string());
    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, true);

    const auto seeded = pulse.seed(fixture.anchor.id);
    expect(seeded.cursor.has_value(), "generation-cap fixture seeds");
    if (!seeded.cursor) return;
    std::int64_t due = seeded.cursor->due_at_ms;
    std::string previous_task;
    for (int generation = 1; generation <= 3; ++generation) {
        fixture.now = time_point(due);
        const auto observed = pulse.observe();
        const auto expected_result = generation == 3
            ? PulseResult::quiescent : PulseResult::settled;
        expect(observed.result == expected_result && observed.cursor && observed.task,
               "generation " + std::to_string(generation)
                   + " settles with expected bounded terminal state");
        if (!observed.cursor || !observed.task || !observed.cursor->captured_at_ms)
            return;
        expect(observed.cursor->generation
                   == static_cast<std::uint64_t>(generation),
               "cursor increments exactly one generation at a time");
        const auto inspected = inspect_local_continuity_observation_task(*observed.task);
        expect(inspected.eligible
                   && inspected.facts.generation
                        == static_cast<std::uint32_t>(generation),
               "each generation has exact canonical payload");
        if (generation > 1) {
            expect(inspected.facts.predecessor_observation_task_id
                       == std::optional<std::string>{previous_task},
                   "later generation chains exact predecessor Task identity");
        }
        previous_task = observed.task->id;
        due = *observed.cursor->captured_at_ms + local_activity_pulse_cadence_ms;
    }

    fixture.now = time_point(due + 10 * local_activity_pulse_cadence_ms);
    expect(pulse.observe().result == PulseResult::quiescent,
           "quiescent generation 3 never schedules generation 4");
    expect(local_observation_count(fixture.files.state) == 3,
           "exactly three local observation Tasks exist after proof cap");
}

void anchored_evidence_drift_blocks_before_task()
{
    Fixture fixture("evidence-drift");
    LocalActivityPulseStore store(fixture.files.sidecar.string());
    gaudere::work::Runtime runtime(
        fixture.tasks, [&fixture] { return fixture.now; });
    runtime.recover();
    LocalActivityPulse pulse(
        store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
        runtime, [&fixture] { return fixture.now; }, true);

    const auto seeded = pulse.seed(fixture.anchor.id);
    expect(seeded.cursor.has_value(), "evidence-drift fixture seeds");
    if (!seeded.cursor) return;

    const auto action_id = provider_action_id(cognition('a'));
    auto action = fixture.actions.find(action_id);
    if (!action) throw std::runtime_error("audited synthetic Action is missing");
    action->status = ActionStatus::manual_review;
    fixture.actions.save(*action);

    fixture.now = time_point(seeded.cursor->due_at_ms);
    const auto observed = pulse.observe();
    expect(observed.result == PulseResult::blocked && observed.cursor,
           "drifted anchored Action blocks local observation fail-closed");
    expect(observed.cursor && observed.cursor->state == LocalActivityPulseState::blocked,
           "evidence conflict persists blocked cursor");
    expect(local_observation_count(fixture.files.state) == 0,
           "evidence drift blocks before any local observation Task is created");
    expect(fixture.provider_total() == 10,
           "evidence-drift handling does not consume provider budget");
}

void conflicting_deterministic_task_blocks()
{
    Fixture fixture("task-conflict");
    bool crashed = false;
    LocalActivityPulseCursor preparing;

    {
        LocalActivityPulseStore store(fixture.files.sidecar.string());
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse pulse(
            store, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; }, true,
            [&crashed](const std::string_view phase) {
                if (!crashed && phase == "after_preparing") {
                    crashed = true;
                    throw std::runtime_error("synthetic crash before Task creation");
                }
            });
        const auto seeded = pulse.seed(fixture.anchor.id);
        expect(seeded.cursor.has_value(), "task-conflict fixture seeds");
        if (!seeded.cursor) return;
        fixture.now = time_point(seeded.cursor->due_at_ms);
        expect(pulse.observe().result == PulseResult::unavailable && crashed,
               "task-conflict fixture freezes deterministic opportunity first");
        const auto found = store.find(local_activity_pulse_scope);
        if (!found) throw std::runtime_error("preparing cursor is missing");
        preparing = *found;
    }

    LocalContinuityObservationFacts conflicting;
    conflicting.generation = static_cast<std::uint32_t>(preparing.generation);
    conflicting.due_at_ms = preparing.due_at_ms;
    conflicting.captured_at_ms = *preparing.captured_at_ms;
    conflicting.predecessor_observation_task_id =
        preparing.predecessor_observation_task_id;
    conflicting.predecessor_observation_result_sha256 =
        preparing.predecessor_observation_result_sha256;
    conflicting.anchor_checkpoint_task_id = preparing.anchor_checkpoint_task_id;
    conflicting.anchor_checkpoint_result_sha256 =
        preparing.anchor_checkpoint_result_sha256;
    conflicting.provider_scope = "provider.call:openai.responses";
    conflicting.provider_total = 9; // deliberately stale; identity does not include facts.
    conflicting.provider_limit = 12;
    conflicting.predecessor_provider_action_id =
        provider_action_id(cognition('b'));
    conflicting.audited_provider_action_id = provider_action_id(cognition('a'));
    conflicting.historical_wake_scope = "cognition.reflect.wake.v0";
    conflicting.historical_wake_sha256 = sha256_hex(empty_wake_canonical());
    auto conflicting_task = make_local_continuity_observation_task(conflicting);
    expect(conflicting_task.id == preparing.task_id,
           "changed captured facts retain the reserved semantic Task identity");
    fixture.tasks.save(conflicting_task);

    {
        LocalActivityPulseStore reopened(fixture.files.sidecar.string());
        gaudere::work::Runtime runtime(
            fixture.tasks, [&fixture] { return fixture.now; });
        runtime.recover();
        LocalActivityPulse pulse(
            reopened, fixture.tasks, fixture.actions, fixture.budgets, fixture.wakes,
            runtime, [&fixture] { return fixture.now; }, true);
        const auto recovered = pulse.observe();
        expect(recovered.result == PulseResult::blocked && recovered.cursor,
               "same deterministic ID with different semantic input fails closed");
        expect(recovered.cursor
                   && recovered.cursor->state == LocalActivityPulseState::blocked,
               "semantic Task conflict persists blocked state");
    }
    expect(local_observation_count(fixture.files.state) == 1,
           "conflict handling never manufactures a second semantic Task");
}

} // namespace

int main()
{
    disabled_and_unseeded_are_inert();
    seed_and_due_observation();
    crash_recovery_matrix();
    five_day_downtime_coalesces();
    clock_rollback_fails_closed();
    three_generations_then_quiescent();
    anchored_evidence_drift_blocks_before_task();
    conflicting_deterministic_task_blocks();

    if (failures != 0) {
        std::cerr << failures << " local activity pulse test(s) failed\n";
        return 1;
    }
    std::cout << "All local activity pulse tests passed\n";
    return 0;
}
