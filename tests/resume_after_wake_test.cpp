#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "ResumeAfterWake.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

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

std::string proposal(const std::uint64_t seconds,
                     const std::string& reason = "Resume this intention once.")
{
    return "{\"decision\":\"propose_wake\",\"reason\":\"" + reason
        + "\",\"schema\":\"gaudere.cognition.decision.v1\","
          "\"wake_after_seconds\":" + std::to_string(seconds) + "}";
}

gaudere::work::Task source_task(std::string id, std::string output)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "durable reflection source fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type, std::move(output), {}, {}};
    return task;
}

gaudere::work::Task unrelated_task(std::string id)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "local.test:" + task.id;
    task.kind = "local.test";
    task.input_content_type = "text/plain";
    task.input = "unchanged";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 1024;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 1;
    return task;
}

gaudere::budget::Policy budget_policy()
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = 24h;
    policy.min_interval = 15min;
    return policy;
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path,
                     gaudere::scheduling::wake::WakeIntentTimePoint initial_now =
                         gaudere::scheduling::wake::WakeIntentTimePoint{1000s})
        : tasks(path.string()),
          wakes(path.string()),
          budgets(path.string()),
          actions(path.string()),
          now(initial_now),
          wake_runtime(wakes, [this] { return now; }, explicit_wake_scope,
                       {explicit_wake_max_total}),
          explicit_wake(tasks, wake_runtime),
          work_runtime(tasks, [this] { return now; })
    {
        work_runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::scheduling::wake::WakeIntentTimePoint now;
    WakeRuntime wake_runtime;
    ExplicitWake explicit_wake;
    gaudere::work::Runtime work_runtime;
};

void create_fired_wake(Fixture& fixture,
                       const std::string& source_id,
                       const std::uint64_t delay = 900)
{
    fixture.tasks.save(source_task(source_id, proposal(delay)));
    const auto accepted = fixture.explicit_wake.accept(source_id);
    if (accepted.result != ExplicitWakeAcceptResult::accepted
        || !accepted.intent) {
        throw std::runtime_error("test fixture could not accept explicit wake");
    }
    fixture.now = accepted.intent->due_at;
    const auto result = fixture.wake_runtime.reconcile();
    if (result.fired != 1) {
        throw std::runtime_error("test fixture could not fire explicit wake");
    }
}

std::string resume_id(const std::string& wake_id)
{
    return std::string{resume_after_wake_task_prefix} + wake_id;
}

std::string provider_action_id(const std::string& wake_id)
{
    return "provider.call:openai.responses:" + resume_id(wake_id);
}

std::string provider_action_key(const std::string& wake_id)
{
    return "provider.call:openai.responses:" + resume_id(wake_id);
}

void expect_no_provider_effect(Fixture& fixture,
                               const std::string& wake_id,
                               const gaudere::budget::Snapshot& before,
                               const std::string& message)
{
    const auto after = fixture.budgets.snapshot(
        "provider.call:openai.responses", fixture.now, budget_policy());
    expect(after.total_used == before.total_used
               && after.in_window_used == before.in_window_used
               && after.last_consumed_at == before.last_consumed_at,
           message + ": provider budget is unchanged");
    expect(!fixture.actions.find(provider_action_id(wake_id))
               && !fixture.actions.find_by_idempotency_key(
                    provider_action_key(wake_id)),
           message + ": no provider Action exists");
}

void test_disabled_is_inert_and_status_is_read_only()
{
    TemporaryDatabase database("resume-disabled");
    Fixture fixture(database.path);
    create_fired_wake(fixture, "wake-disabled");
    const auto budget_before = fixture.budgets.snapshot(
        "provider.call:openai.responses", fixture.now, budget_policy());

    ResumeAfterWake resume(
        fixture.tasks, fixture.explicit_wake, fixture.work_runtime, false);
    const auto first = resume.inspect("wake-disabled");
    const auto second = resume.inspect("wake-disabled");
    const auto claim = resume.claim("wake-disabled");

    expect(first.state == ResumeAfterWakeState::disabled && first.healthy
               && second.report == first.report,
           "disabled status is stable and read-only");
    expect(claim.result == ResumeAfterWakeClaimResult::disabled
               && !claim.task
               && !fixture.tasks.find(resume_id("wake-disabled")),
           "disabled capability cannot synthesize a resume Task");
    expect_no_provider_effect(
        fixture, "wake-disabled", budget_before,
        "disabled capability has zero provider effect");
}

void test_one_claim_repeated_reconciliation_and_unrelated_state()
{
    TemporaryDatabase database("resume-one-claim");
    Fixture fixture(database.path);
    create_fired_wake(fixture, "wake-one");
    const auto unrelated = unrelated_task("unrelated-pending");
    fixture.tasks.save(unrelated);
    const auto budget_before = fixture.budgets.snapshot(
        "provider.call:openai.responses", fixture.now, budget_policy());

    ResumeAfterWake resume(
        fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
    const auto before = resume.inspect("wake-one");
    expect(before.state == ResumeAfterWakeState::eligible && before.healthy
               && !fixture.tasks.find(resume_id("wake-one")),
           "valid fired lineage is read-only eligible before claim");

    const auto accepted = resume.claim("wake-one");
    expect(accepted.result == ResumeAfterWakeClaimResult::accepted
               && accepted.task
               && accepted.task->id == resume_id("wake-one")
               && accepted.task->idempotency_key == resume_id("wake-one")
               && accepted.task->kind == resume_after_wake_task_kind
               && accepted.task->status == gaudere::work::TaskStatus::pending
               && accepted.task->attempts_started == 0
               && accepted.task->input.find(resume_after_wake_context_schema)
                    != std::string::npos
               && accepted.task->input.find("wake-one") != std::string::npos,
           "eligible fired lineage creates one deterministic pending resume Task");

    const auto canonical_input = accepted.task ? accepted.task->input : std::string{};
    for (int iteration = 0; iteration < 100; ++iteration) {
        const auto duplicate = resume.claim("wake-one");
        expect(duplicate.result == ResumeAfterWakeClaimResult::duplicate
                   && duplicate.task
                   && duplicate.task->id == resume_id("wake-one")
                   && duplicate.task->input == canonical_input,
               "repeated reconciliation preserves one exact resume Task");
    }

    const auto after = resume.inspect("wake-one");
    expect(after.state == ResumeAfterWakeState::claimed && after.healthy
               && after.report.find("resume_task_status=pending")
                    != std::string::npos,
           "status derives claimed state from durable Task without mutation");

    const auto unrelated_after = fixture.tasks.find(unrelated.id);
    expect(unrelated_after
               && unrelated_after->status == unrelated.status
               && unrelated_after->attempts_started == unrelated.attempts_started
               && unrelated_after->input == unrelated.input,
           "resume claim does not alter unrelated durable Task state");
    expect_no_provider_effect(
        fixture, "wake-one", budget_before,
        "provider-free repeated claim has zero provider effect");
}

void test_reopen_before_and_after_claim()
{
    TemporaryDatabase database("resume-reopen");

    {
        Fixture fixture(database.path);
        create_fired_wake(fixture, "wake-reopen");
        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.inspect("wake-reopen").state
                   == ResumeAfterWakeState::eligible,
               "pre-submit observation can end without creating a claim");
        expect(!fixture.tasks.find(resume_id("wake-reopen")),
               "simulated crash before submit leaves no resume Task");
    }

    {
        Fixture reopened(database.path,
                         gaudere::scheduling::wake::WakeIntentTimePoint{3000s});
        ResumeAfterWake resume(
            reopened.tasks, reopened.explicit_wake, reopened.work_runtime, true);
        const auto accepted = resume.claim("wake-reopen");
        expect(accepted.result == ResumeAfterWakeClaimResult::accepted,
               "reopen after pre-submit crash safely creates one claim");
    }

    {
        Fixture reopened(database.path,
                         gaudere::scheduling::wake::WakeIntentTimePoint{4000s});
        ResumeAfterWake resume(
            reopened.tasks, reopened.explicit_wake, reopened.work_runtime, true);
        const auto duplicate = resume.claim("wake-reopen");
        expect(duplicate.result == ResumeAfterWakeClaimResult::duplicate
                   && duplicate.task
                   && duplicate.task->id == resume_id("wake-reopen"),
               "reopen after durable save observes duplicate rather than new claim");
        expect(resume.inspect("wake-reopen").state
                   == ResumeAfterWakeState::claimed,
               "reopened claim remains durably observable");
    }
}

void test_non_fired_and_missing_wakes_fail_closed()
{
    {
        TemporaryDatabase database("resume-scheduled");
        Fixture fixture(database.path);
        fixture.tasks.save(source_task("wake-scheduled", proposal(900)));
        expect(fixture.explicit_wake.accept("wake-scheduled").result
                   == ExplicitWakeAcceptResult::accepted,
               "scheduled fixture accepts wake");
        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.claim("wake-scheduled").result
                   == ResumeAfterWakeClaimResult::ineligible
                   && !fixture.tasks.find(resume_id("wake-scheduled")),
               "scheduled wake cannot grant resume authority");
        expect(resume.claim("missing").result
                   == ResumeAfterWakeClaimResult::wake_not_found,
               "missing wake cannot grant resume authority");
    }

    {
        TemporaryDatabase database("resume-revoked");
        Fixture fixture(database.path);
        fixture.tasks.save(source_task("wake-revoked", proposal(900)));
        expect(fixture.explicit_wake.accept("wake-revoked").result
                   == ExplicitWakeAcceptResult::accepted,
               "revoked fixture accepts wake");
        fixture.now += 1s;
        expect(fixture.explicit_wake.revoke("wake-revoked", "test revocation")
                   == gaudere::scheduling::wake::WakeIntentRevokeResult::revoked,
               "revoked fixture reaches terminal revoked state");
        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.claim("wake-revoked").result
                   == ResumeAfterWakeClaimResult::ineligible
                   && !fixture.tasks.find(resume_id("wake-revoked")),
               "revoked wake cannot grant resume authority");
    }
}

void test_source_drift_and_task_conflicts_fail_closed()
{
    {
        TemporaryDatabase database("resume-source-drift");
        Fixture fixture(database.path);
        create_fired_wake(fixture, "wake-source-drift");
        auto source = *fixture.tasks.find("wake-source-drift");
        source.kind = "local.echo";
        fixture.tasks.save(source);

        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.claim("wake-source-drift").result
                   == ResumeAfterWakeClaimResult::ineligible
                   && !fixture.tasks.find(resume_id("wake-source-drift")),
               "source drift after firing fails closed without a claim");
    }

    {
        TemporaryDatabase database("resume-id-conflict");
        Fixture fixture(database.path);
        create_fired_wake(fixture, "wake-id-conflict");
        auto conflict = unrelated_task(resume_id("wake-id-conflict"));
        conflict.idempotency_key = conflict.id;
        fixture.tasks.save(conflict);

        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.claim("wake-id-conflict").result
                   == ResumeAfterWakeClaimResult::conflict
                   && resume.inspect("wake-id-conflict").state
                        == ResumeAfterWakeState::manual_review,
               "same deterministic ID with different definition fails closed");
    }

    {
        TemporaryDatabase database("resume-key-conflict");
        Fixture fixture(database.path);
        create_fired_wake(fixture, "wake-key-conflict");
        auto conflict = unrelated_task("different-task-id");
        conflict.idempotency_key = resume_id("wake-key-conflict");
        fixture.tasks.save(conflict);

        ResumeAfterWake resume(
            fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
        expect(resume.claim("wake-key-conflict").result
                   == ResumeAfterWakeClaimResult::conflict
                   && !fixture.tasks.find(resume_id("wake-key-conflict")),
               "idempotency key owned by another Task fails closed");
    }
}

void test_derived_lifecycle_status_without_provider()
{
    TemporaryDatabase database("resume-lifecycle");
    Fixture fixture(database.path);
    create_fired_wake(fixture, "wake-lifecycle");
    ResumeAfterWake resume(
        fixture.tasks, fixture.explicit_wake, fixture.work_runtime, true);
    const auto claim = resume.claim("wake-lifecycle");
    expect(claim.result == ResumeAfterWakeClaimResult::accepted,
           "lifecycle fixture claims resume Task");

    expect(fixture.work_runtime.start(resume_id("wake-lifecycle"), "test-worker"),
           "synthetic worker starts resume Task without provider");
    expect(resume.inspect("wake-lifecycle").state
               == ResumeAfterWakeState::claimed,
           "running resume Task remains claimed");
    expect(fixture.work_runtime.require_manual_review(
               resume_id("wake-lifecycle"), "synthetic_ambiguity",
               "provider-free synthetic ambiguity"),
           "synthetic ambiguity can terminalize Task for status proof");
    const auto status = resume.inspect("wake-lifecycle");
    expect(status.state == ResumeAfterWakeState::manual_review && !status.healthy,
           "manual-review Task is derived as manual_review without replay");
}

} // namespace

int main()
{
    test_disabled_is_inert_and_status_is_read_only();
    test_one_claim_repeated_reconciliation_and_unrelated_state();
    test_reopen_before_and_after_claim();
    test_non_fired_and_missing_wakes_fail_closed();
    test_source_drift_and_task_conflicts_fail_closed();
    test_derived_lifecycle_status_without_provider();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All provider-free resume-after-wake tests passed\n";
    return 0;
}
