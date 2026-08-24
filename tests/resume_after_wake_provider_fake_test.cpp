#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "ProviderTaskHandler.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;
using ActionStatus = gaudere::scheduling::wake::ActionStatus;
using EffectResult = gaudere::scheduling::wake::EffectResult;
using TaskStatus = gaudere::work::TaskStatus;

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
            / ("gaudere-agent-resume-provider-fake-" + std::move(label) + "-"
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

std::string proposal(const std::uint64_t seconds = 900)
{
    return "{\"decision\":\"propose_wake\",\"reason\":\"Resume this intention once.\","
           "\"schema\":\"gaudere.cognition.decision.v1\",\"wake_after_seconds\":"
        + std::to_string(seconds) + "}";
}

gaudere::work::Task source_task(std::string id)
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
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type, proposal(), {}, {}};
    return task;
}

gaudere::budget::Policy budget_policy()
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = 24h;
    policy.min_interval = 0ms;
    return policy;
}

class FakeResumeProvider final : public Provider {
public:
    std::string_view name() const noexcept override { return "fake-resume"; }

    ProviderResult invoke(const ProviderRequest& request) override
    {
        ++calls;
        last_request = request;
        if (throw_exception) {
            throw std::runtime_error("synthetic fake provider exception");
        }
        return next_result;
    }

    int calls = 0;
    bool throw_exception = false;
    ProviderResult next_result;
    std::optional<ProviderRequest> last_request;
};

struct Harness {
    explicit Harness(const std::filesystem::path& path)
        : tasks(path.string()),
          wakes(path.string()),
          actions(path.string()),
          budgets(path.string()),
          wake_now(gaudere::scheduling::wake::WakeIntentTimePoint{1000s}),
          work_now(gaudere::work::TimePoint{1000s}),
          action_now(gaudere::scheduling::wake::TimePoint{1000s}),
          budget_now(gaudere::budget::TimePoint{1000s}),
          wake_runtime(wakes, [this] { return wake_now; }, explicit_wake_scope,
                       {explicit_wake_max_total}),
          explicit_wake(tasks, wake_runtime),
          work_runtime(tasks, [this] { return work_now; }),
          action_runtime(actions, [this] { return action_now; }),
          provider_handler(action_runtime, actions, provider, budgets,
                           budget_policy(), [this] { return budget_now; }),
          cognition_handler(provider_handler),
          executor(work_runtime, tasks)
    {
        work_runtime.recover();
        action_runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::scheduling::wake::WakeIntentTimePoint wake_now;
    gaudere::work::TimePoint work_now;
    gaudere::scheduling::wake::TimePoint action_now;
    gaudere::budget::TimePoint budget_now;
    gaudere::scheduling::wake::WakeIntentRuntime wake_runtime;
    ExplicitWake explicit_wake;
    gaudere::work::Runtime work_runtime;
    gaudere::scheduling::wake::Runtime action_runtime;
    FakeResumeProvider provider;
    ProviderTaskHandler provider_handler;
    ResumeAfterWakeCognitionHandler cognition_handler;
    TaskExecutor executor;
};

std::string prepare_resume(Harness& harness, const std::string& source_id)
{
    harness.tasks.save(source_task(source_id));
    const auto accepted = harness.explicit_wake.accept(source_id);
    if (accepted.result != ExplicitWakeAcceptResult::accepted
        || !accepted.intent) {
        throw std::runtime_error("fixture could not accept wake");
    }
    harness.wake_now = accepted.intent->due_at;
    const auto fired = harness.wake_runtime.reconcile();
    if (fired.fired != 1) {
        throw std::runtime_error("fixture could not fire wake");
    }

    ResumeAfterWake resume(
        harness.tasks, harness.wakes, harness.work_runtime, true);
    const auto claim = resume.claim(source_id);
    if (claim.result != ResumeAfterWakeClaimResult::accepted || !claim.task) {
        throw std::runtime_error("fixture could not claim resume Task");
    }
    return claim.task->id;
}

std::string action_id(const std::string& task_id)
{
    return "provider.call:fake-resume:" + task_id;
}

std::string action_key(const std::string& task_id)
{
    return "provider.call:fake-resume:" + task_id;
}

void test_stop_and_continue_are_canonical_and_durable()
{
    {
        TemporaryDatabase database("stop");
        Harness harness(database.path);
        const auto task_id = prepare_resume(harness, "wake-stop");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            "{\"reason\":\"The intended observation is complete.\",\"decision\":\"stop\","
            "\"schema\":\"gaudere.cognition.resume-decision.v1\"}",
            {}, {}};

        expect(harness.executor.execute(
                   task_id, "fake-resume-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "stop resume Task completes through fake provider");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->content_type
                        == resume_after_wake_decision_content_type
                   && done->result->output
                        == "{\"decision\":\"stop\",\"reason\":\"The intended observation is complete.\","
                           "\"schema\":\"gaudere.cognition.resume-decision.v1\"}",
               "stop proposal is canonical durable output");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "stop fake provider call has confirmed durable Action evidence");
        const auto budget = harness.budgets.snapshot(
            "provider.call:fake-resume", harness.budget_now, budget_policy());
        expect(budget.total_used == 1 && harness.provider.calls == 1,
               "stop path consumes exactly one fake provider permit/call");
    }

    {
        TemporaryDatabase database("continue");
        Harness harness(database.path);
        const auto task_id = prepare_resume(harness, "wake-continue");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            "{\"objective\":\"Measure whether the resumed thread remains interpretable.\","
            "\"schema\":\"gaudere.cognition.resume-decision.v1\","
            "\"reason\":\"The wake evidence is valid and the thread still has one bounded next step.\","
            "\"decision\":\"continue\"}",
            {}, {}};

        expect(harness.executor.execute(
                   task_id, "fake-resume-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "continue resume Task completes through fake provider");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->content_type
                        == resume_after_wake_decision_content_type
                   && done->result->output.find("\"decision\":\"continue\"")
                        != std::string::npos
                   && done->result->output.find(
                        "\"objective\":\"Measure whether the resumed thread remains interpretable.\"")
                        != std::string::npos,
               "continue proposal is normalized and durable");
        expect(harness.provider.last_request
                   && harness.provider.last_request->idempotency_key
                        == action_key(task_id)
                   && harness.provider.last_request->max_output_bytes == 8 * 1024
                   && harness.provider.last_request->max_runtime == 60s,
               "resume Task bounds and deterministic key reach fake provider");
    }
}

void test_invalid_model_output_fails_closed_after_one_confirmed_call()
{
    const std::string invalid_outputs[] = {
        "not-json",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\","
        "\"reason\":\"one\",\"reason\":\"two\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"continue\","
        "\"reason\":\"missing objective\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"act\","
        "\"reason\":\"unsupported authority\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\","
        "\"reason\":\"extra key\",\"extra\":true}"
    };

    int index = 0;
    for (const auto& output : invalid_outputs) {
        TemporaryDatabase database("invalid-" + std::to_string(index));
        Harness harness(database.path);
        const auto task_id = prepare_resume(
            harness, "wake-invalid-" + std::to_string(index));
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain", output, {}, {}};

        expect(harness.executor.execute(
                   task_id, "fake-resume-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "invalid fake-provider output closes Task lifecycle");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::failed && done->result
                   && done->result->failure_code
                        == "cognition_invalid_resume_decision",
               "invalid resume proposal fails closed");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed
                   && harness.provider.calls == 1,
               "invalid proposal does not erase confirmed one-call evidence");
        ++index;
    }
}

void test_effect_unknown_and_exception_never_replay()
{
    {
        TemporaryDatabase database("unknown");
        Harness harness(database.path);
        const auto task_id = prepare_resume(harness, "wake-unknown");
        const auto task = harness.tasks.find(task_id);
        if (!task) {
            throw std::runtime_error("unknown fixture lacks resume Task");
        }
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::effect_unknown, {}, {},
            "fake_resume_unknown", "synthetic transport ambiguity"};
        const TaskContext context{*task, [] { return false; }};

        const auto first = harness.cognition_handler.execute(context);
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::manual_review
                   && first.failure_code == "fake_resume_unknown"
                   && second.outcome == HandlerOutcome::manual_review
                   && harness.provider.calls == 1,
               "ambiguous fake provider effect is never replayed");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::manual_review
                   && action->effect_result == EffectResult::unknown,
               "ambiguous fake provider Action remains durable manual review");
    }

    {
        TemporaryDatabase database("exception");
        Harness harness(database.path);
        const auto task_id = prepare_resume(harness, "wake-exception");
        const auto task = harness.tasks.find(task_id);
        if (!task) {
            throw std::runtime_error("exception fixture lacks resume Task");
        }
        harness.provider.throw_exception = true;
        const TaskContext context{*task, [] { return false; }};

        const auto first = harness.cognition_handler.execute(context);
        harness.provider.throw_exception = false;
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain",
            "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
            "\"decision\":\"stop\",\"reason\":\"must not replay\"}", {}, {}};
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::manual_review
                   && first.failure_code == "provider_exception"
                   && second.outcome == HandlerOutcome::manual_review
                   && harness.provider.calls == 1,
               "provider exception leaves no automatic retry path");
    }
}

void test_confirmed_call_crash_window_becomes_manual_review_without_recall()
{
    TemporaryDatabase database("confirmed-crash");
    Harness harness(database.path);
    const auto task_id = prepare_resume(harness, "wake-confirmed-crash");
    const auto task = harness.tasks.find(task_id);
    if (!task) {
        throw std::runtime_error("confirmed crash fixture lacks resume Task");
    }
    harness.provider.next_result = ProviderResult{
        ProviderOutcome::succeeded, "text/plain",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"stop\",\"reason\":\"definite response before synthetic crash\"}",
        {}, {}};
    const TaskContext context{*task, [] { return false; }};

    const auto first = harness.cognition_handler.execute(context);
    expect(first.outcome == HandlerOutcome::succeeded
               && first.content_type == resume_after_wake_decision_content_type
               && harness.provider.calls == 1,
           "first direct handler call obtains one definite normalized response");

    // Simulate a crash after provider confirmation/handler return but before the
    // TaskExecutor can persist the returned Task result. Re-entering the same
    // deterministic Task must observe the confirmed Action and refuse recall.
    const auto second = harness.cognition_handler.execute(context);
    expect(second.outcome == HandlerOutcome::manual_review
               && second.failure_code == "provider_response_not_durable"
               && harness.provider.calls == 1,
           "confirmed-response crash window fails to manual review without recall");
    const auto action = harness.actions.find(action_id(task_id));
    expect(action && action->status == ActionStatus::succeeded
               && action->effect_result == EffectResult::confirmed,
           "confirmed provider evidence survives synthetic task-result crash");
    const auto budget = harness.budgets.snapshot(
        "provider.call:fake-resume", harness.budget_now, budget_policy());
    expect(budget.total_used == 1,
           "confirmed-response crash window consumes exactly one durable budget permit");
}

} // namespace

int main()
{
    test_stop_and_continue_are_canonical_and_durable();
    test_invalid_model_output_fails_closed_after_one_confirmed_call();
    test_effect_unknown_and_exception_never_replay();
    test_confirmed_call_crash_window_becomes_manual_review_without_recall();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All fake-provider resume-after-wake integration tests passed\n";
    return 0;
}
