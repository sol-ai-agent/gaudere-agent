#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "ProviderTaskHandler.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeAfterWakeV1.hpp"
#include "ResumeAfterWakeV1Cognition.hpp"
#include "ResumeContextSnapshot.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
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
            / ("gaudere-agent-resume-v1-provider-fake-" + std::move(label) + "-"
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

gaudere::budget::Policy budget_policy()
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = 24h;
    policy.min_interval = 0ms;
    return policy;
}

std::string source_output(const std::uint64_t seconds = 900)
{
    return Json{{"decision", "propose_wake"},
                {"reason",
                 "Resume after observation to verify durable wake evidence, journal it, and identify the single reliability condition for future WakeIntent enablement."},
                {"schema", "gaudere.cognition.decision.v1"},
                {"wake_after_seconds", seconds}}.dump();
}

gaudere::work::Task source_task(std::string id)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "historical reflection fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type, source_output(), {}, {}};
    return task;
}

std::string snapshot_request(const std::string& content)
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/markdown; charset=utf-8"},
        {"content", content},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", "fake-provider-v1-proof"},
            {"sha256", std::string(64, '1')}
        }})}
    }.dump();
}

class FakeResumeV1Provider final : public Provider {
public:
    std::string_view name() const noexcept override { return "fake-resume-v1"; }

    ProviderResult invoke(const ProviderRequest& request) override
    {
        ++calls;
        last_request = request;
        if (throw_exception) {
            throw std::runtime_error("synthetic v1 provider exception");
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
        : tasks(path.string()), wakes(path.string()), actions(path.string()),
          budgets(path.string()),
          wake_now(gaudere::scheduling::wake::WakeIntentTimePoint{1000s}),
          work_now(gaudere::work::TimePoint{1000s}),
          action_now(gaudere::scheduling::wake::TimePoint{1000s}),
          budget_now(gaudere::budget::TimePoint{1000s}),
          wake_runtime(wakes, [this] { return wake_now; },
                       bounded_reflection_wake_scope, {1}),
          explicit_wake(tasks, wake_runtime),
          work_runtime(tasks, [this] { return work_now; }),
          action_runtime(actions, [this] { return action_now; }),
          recorder(tasks, work_runtime, [this] { return work_now; }),
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
    ResumeContextSnapshotRecorder recorder;
    FakeResumeV1Provider provider;
    ProviderTaskHandler provider_handler;
    ResumeAfterWakeV1CognitionHandler cognition_handler;
    TaskExecutor executor;
};

void synchronize_after_wake(Harness& harness)
{
    const auto elapsed = harness.wake_now.time_since_epoch();
    harness.work_now = gaudere::work::TimePoint{elapsed};
    harness.action_now = gaudere::scheduling::wake::TimePoint{elapsed};
    harness.budget_now = gaudere::budget::TimePoint{elapsed};
}

std::string prepare_resume_v1(Harness& harness,
                              const std::string& source_id,
                              const std::string& fresh_content)
{
    harness.tasks.save(source_task(source_id));
    const auto accepted = harness.explicit_wake.accept(source_id);
    if (accepted.result != ExplicitWakeAcceptResult::accepted || !accepted.intent) {
        throw std::runtime_error("fixture could not accept wake");
    }
    harness.wake_now = accepted.intent->due_at;
    if (harness.wake_runtime.reconcile().fired != 1) {
        throw std::runtime_error("fixture could not fire wake");
    }
    synchronize_after_wake(harness);
    harness.work_now += 1min;
    harness.action_now += 1min;
    harness.budget_now += 1min;

    const auto snapshot = harness.recorder.record(snapshot_request(fresh_content));
    if (snapshot.result != ResumeContextSnapshotRecordResult::accepted
        || !snapshot.task) {
        throw std::runtime_error("fixture could not record fresh context: "
                                 + snapshot.detail);
    }

    ResumeAfterWakeV1 resume(
        harness.tasks, harness.wakes, harness.work_runtime,
        [&harness] { return harness.work_now; }, true);
    const auto claim = resume.claim(source_id, snapshot.task->id);
    if (claim.result != ResumeAfterWakeV1ClaimResult::accepted || !claim.task) {
        throw std::runtime_error("fixture could not claim resume v1 Task: "
                                 + claim.detail);
    }
    return claim.task->id;
}

std::string action_id(const std::string& task_id)
{
    return "provider.call:fake-resume-v1:" + task_id;
}

void expect_fresh_context_reaches_provider(const Harness& harness,
                                           const std::string& phrase,
                                           const std::string& label)
{
    expect(harness.provider.last_request.has_value(),
           label + ": provider request exists");
    if (!harness.provider.last_request) return;
    const auto& request = *harness.provider.last_request;
    expect(request.content_type == resume_after_wake_v1_content_type,
           label + ": canonical v1 content type reaches provider");
    expect(request.input.find("\"historical\"") != std::string::npos
               && request.input.find("\"current_context\"") != std::string::npos,
           label + ": historical and current-context blocks both reach provider");
    expect(request.input.find(phrase) != std::string::npos,
           label + ": frozen fresh-context fact reaches provider");
    expect(request.input.find("verify durable wake evidence") != std::string::npos,
           label + ": immutable historical intention also reaches provider");
}

void test_fresh_context_can_stop_stale_historical_objective()
{
    TemporaryDatabase database("stop-stale");
    Harness harness(database.path);
    const auto task_id = prepare_resume_v1(
        harness, "wake-v1-stop",
        "Current state: wake proof PASS; runtime-downtime PASS; journal DONE; "
        "reliability condition DONE. The historical verification objective is already complete.");

    harness.provider.next_result = ProviderResult{
        ProviderOutcome::succeeded, "text/plain",
        "{\"reason\":\"Fresh durable context shows the historical verification objective is already complete.\","
        "\"decision\":\"stop\",\"schema\":\"gaudere.cognition.resume-decision.v1\"}",
        {}, {}};

    expect(harness.executor.execute(
               task_id, "fake-resume-v1-worker", harness.cognition_handler)
               == ExecuteResult::completed,
           "fresh-context stop completes through fake provider");
    const auto done = harness.tasks.find(task_id);
    expect(done && done->status == TaskStatus::succeeded && done->result
               && done->result->content_type == resume_after_wake_decision_content_type
               && done->result->output.find("\"decision\":\"stop\"")
                    != std::string::npos,
           "stale historical objective can normalize to durable stop");
    expect_fresh_context_reaches_provider(
        harness, "historical verification objective is already complete",
        "fresh-context stop");
    const auto action = harness.actions.find(action_id(task_id));
    expect(action && action->status == ActionStatus::succeeded
               && action->effect_result == EffectResult::confirmed
               && harness.provider.calls == 1,
           "fresh-context stop has exactly one confirmed fake provider call");
}

void test_fresh_context_can_continue_with_new_objective()
{
    TemporaryDatabase database("continue-new");
    Harness harness(database.path);
    const auto task_id = prepare_resume_v1(
        harness, "wake-v1-continue",
        "Current state: historical wake verification is DONE. A new bounded open thread is "
        "to test whether fresh-context cognition rejects malformed Task kinds before provider invocation.");

    harness.provider.next_result = ProviderResult{
        ProviderOutcome::succeeded, "text/plain",
        "{\"objective\":\"Prove the v1 cognition guard rejects non-canonical Task input before any provider effect.\","
        "\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"reason\":\"The historical objective is complete but the fresh context exposes one new bounded reliability step.\","
        "\"decision\":\"continue\"}", {}, {}};

    expect(harness.executor.execute(
               task_id, "fake-resume-v1-worker", harness.cognition_handler)
               == ExecuteResult::completed,
           "fresh-context continue completes through fake provider");
    const auto done = harness.tasks.find(task_id);
    expect(done && done->status == TaskStatus::succeeded && done->result
               && done->result->output.find("\"decision\":\"continue\"")
                    != std::string::npos
               && done->result->output.find("v1 cognition guard") != std::string::npos,
           "fresh context can produce a genuinely new bounded objective");
    expect_fresh_context_reaches_provider(
        harness, "historical wake verification is DONE",
        "fresh-context continue");
}

void test_invalid_task_context_is_rejected_before_provider()
{
    TemporaryDatabase database("invalid-context");
    Harness harness(database.path);
    const auto task_id = prepare_resume_v1(
        harness, "wake-v1-invalid-context", "Current state is bounded and fresh.");
    const auto valid = harness.tasks.find(task_id);
    if (!valid) throw std::runtime_error("valid v1 fixture missing");

    const auto expect_rejected = [&](gaudere::work::Task task,
                                     const std::string& label) {
        const TaskContext context{task, [] { return false; }};
        const auto before = harness.provider.calls;
        const auto result = harness.cognition_handler.execute(context);
        expect(result.outcome == HandlerOutcome::failed
                   && result.failure_code == "cognition_invalid_resume_context",
               label + ": non-canonical Task fails closed");
        expect(harness.provider.calls == before,
               label + ": provider is not invoked");
    };

    auto wrong_kind = *valid;
    wrong_kind.kind = "local.unrelated";
    expect_rejected(std::move(wrong_kind), "wrong Task kind");

    auto malformed = *valid;
    malformed.input = "not-json";
    expect_rejected(std::move(malformed), "malformed v2 input");

    auto unknown_key = *valid;
    auto parsed = Json::parse(unknown_key.input);
    parsed["unexpected"] = true;
    unknown_key.input = parsed.dump();
    expect_rejected(std::move(unknown_key), "unknown v2 root key");

    auto altered_snapshot = *valid;
    parsed = Json::parse(altered_snapshot.input);
    parsed["current_context"]["capsule"]["content"] = "tampered after hash";
    altered_snapshot.input = parsed.dump();
    expect_rejected(std::move(altered_snapshot), "snapshot/hash mismatch");
}

void test_invalid_provider_output_remains_fail_closed()
{
    const std::string outputs[] = {
        "not-json",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\","
        "\"reason\":\"one\",\"reason\":\"two\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"continue\","
        "\"reason\":\"missing objective\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"act\","
        "\"reason\":\"unsupported authority\"}",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\","
        "\"reason\":\"extra\",\"unknown\":true}"
    };

    int index = 0;
    for (const auto& output : outputs) {
        TemporaryDatabase database("invalid-output-" + std::to_string(index));
        Harness harness(database.path);
        const auto task_id = prepare_resume_v1(
            harness, "wake-v1-invalid-output-" + std::to_string(index),
            "Current state fixture remains fresh.");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain", output, {}, {}};
        expect(harness.executor.execute(
                   task_id, "fake-resume-v1-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "invalid provider output closes Task lifecycle");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::failed && done->result
                   && done->result->failure_code
                        == "cognition_invalid_resume_decision",
               "invalid v1 provider proposal fails closed through shared normalizer");
        expect(harness.provider.calls == 1,
               "invalid provider proposal is not retried");
        ++index;
    }
}

void test_ambiguous_and_confirmed_crash_windows_never_replay()
{
    {
        TemporaryDatabase database("effect-unknown");
        Harness harness(database.path);
        const auto task_id = prepare_resume_v1(
            harness, "wake-v1-unknown", "Current state fixture is fresh.");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("unknown fixture missing Task");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::effect_unknown, {}, {},
            "fake_resume_v1_unknown", "synthetic transport ambiguity"};
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::manual_review
                   && second.outcome == HandlerOutcome::manual_review
                   && harness.provider.calls == 1,
               "ambiguous v1 fake-provider effect is never replayed");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::manual_review
                   && action->effect_result == EffectResult::unknown,
               "ambiguous v1 Action remains durable manual review");
    }

    {
        TemporaryDatabase database("exception");
        Harness harness(database.path);
        const auto task_id = prepare_resume_v1(
            harness, "wake-v1-exception", "Current state fixture is fresh.");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("exception fixture missing Task");
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
               "v1 provider exception leaves no automatic replay path");
    }

    {
        TemporaryDatabase database("confirmed-before-task-result");
        Harness harness(database.path);
        const auto task_id = prepare_resume_v1(
            harness, "wake-v1-confirmed-crash", "Current state fixture is fresh.");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("confirmed fixture missing Task");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain",
            "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
            "\"decision\":\"stop\",\"reason\":\"definite response before synthetic crash\"}",
            {}, {}};
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::succeeded
                   && second.outcome == HandlerOutcome::manual_review
                   && second.failure_code == "provider_response_not_durable"
                   && harness.provider.calls == 1,
               "confirmed v1 provider response not durably attached to Task is never recalled");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "confirmed external effect remains durable across synthetic crash window");
    }
}

} // namespace

int main()
{
    try {
        test_fresh_context_can_stop_stale_historical_objective();
        test_fresh_context_can_continue_with_new_objective();
        test_invalid_task_context_is_rejected_before_provider();
        test_invalid_provider_output_remains_fail_closed();
        test_ambiguous_and_confirmed_crash_windows_never_replay();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " resume-after-wake v1 fake-provider assertion(s) failed\n";
        return 1;
    }
    std::cout << "resume-after-wake v1 fake-provider tests: PASS\n";
    return 0;
}
