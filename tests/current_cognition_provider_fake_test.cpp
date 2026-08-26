#include "CurrentCognitionCycle.hpp"
#include "CurrentCognitionHandler.hpp"
#include "ProviderTaskHandler.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;
using namespace std::chrono_literals;
using ActionStatus = gaudere::scheduling::wake::ActionStatus;
using EffectResult = gaudere::scheduling::wake::EffectResult;
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

struct TemporaryDatabase {
    explicit TemporaryDatabase(std::string label)
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-current-provider-fake-" + std::move(label) + "-"
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
                {"reason", "Historical continuity leaves one bounded next step."},
                {"objective", objective}}.dump();
}

Task predecessor_task(const std::string& id)
{
    Task task;
    task.id = id;
    task.idempotency_key = id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "historical bootstrap cognition";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type,
        decision_continue("Choose the next objective from fresh durable evidence."),
        {}, {}};
    return task;
}

std::string snapshot_request(const std::string& content,
                             const std::string& ref)
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

gaudere::budget::Policy budget_policy()
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = 24h;
    policy.min_interval = 0ms;
    return policy;
}

class FakeCurrentProvider final : public Provider {
public:
    std::string_view name() const noexcept override { return "fake-current"; }

    ProviderResult invoke(const ProviderRequest& request) override
    {
        ++calls;
        last_request = request;
        if (throw_exception) {
            throw std::runtime_error("synthetic current cognition provider exception");
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
          actions(path.string()),
          budgets(path.string()),
          wakes(path.string()),
          work_now(gaudere::work::TimePoint{1000s}),
          action_now(gaudere::scheduling::wake::TimePoint{1000s}),
          budget_now(gaudere::budget::TimePoint{1000s}),
          work_runtime(tasks, [this] { return work_now; }),
          action_runtime(actions, [this] { return action_now; }),
          recorder(tasks, work_runtime, [this] { return work_now; }),
          cycle(tasks, work_runtime, [this] { return work_now; }, true),
          provider_handler(action_runtime, actions, provider, budgets,
                           budget_policy(), [this] { return budget_now; }),
          cognition_handler(provider_handler),
          executor(work_runtime, tasks)
    {
        work_runtime.recover();
        action_runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint work_now;
    gaudere::scheduling::wake::TimePoint action_now;
    gaudere::budget::TimePoint budget_now;
    gaudere::work::Runtime work_runtime;
    gaudere::scheduling::wake::Runtime action_runtime;
    ResumeContextSnapshotRecorder recorder;
    CurrentCognitionCycle cycle;
    FakeCurrentProvider provider;
    ProviderTaskHandler provider_handler;
    CurrentCognitionHandler cognition_handler;
    TaskExecutor executor;
};

std::string prepare_current(Harness& harness, const std::string& label)
{
    const auto predecessor = predecessor_task("bootstrap-" + label);
    harness.tasks.save(predecessor);
    const auto recorded = harness.recorder.record(snapshot_request(
        "Fresh durable evidence for " + label
            + ": earlier bootstrap work is historical; choose only one current bounded objective.",
        "current-provider-" + label));
    if ((recorded.result != ResumeContextSnapshotRecordResult::accepted
         && recorded.result != ResumeContextSnapshotRecordResult::duplicate)
        || !recorded.task) {
        throw std::runtime_error("could not record current snapshot: " + recorded.detail);
    }
    const auto claim = harness.cycle.claim(predecessor.id, recorded.task->id);
    if ((claim.result != CurrentCognitionClaimResult::accepted
         && claim.result != CurrentCognitionClaimResult::duplicate)
        || !claim.task) {
        throw std::runtime_error("could not claim current cognition: " + claim.detail);
    }
    return claim.task->id;
}

std::string action_id(const std::string& task_id)
{
    return "provider.call:fake-current:" + task_id;
}

void expect_no_wake(const TemporaryDatabase& database, const std::string& label)
{
    expect(count_rows(database.path, "wake_intents") == 0,
           label + ": current cognition creates no WakeIntent");
}

void test_stop_and_continue_are_durable_and_input_is_exact()
{
    {
        TemporaryDatabase database("stop");
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "stop");
        const auto before = harness.tasks.find(task_id);
        if (!before) throw std::runtime_error("stop fixture lacks current Task");
        const auto exact_input = before->input;
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            Json{{"schema", resume_after_wake_decision_schema},
                 {"decision", "stop"},
                 {"reason", "Fresh evidence leaves no useful bounded objective."}}.dump(),
            {}, {}};

        expect(harness.executor.execute(
                   task_id, "fake-current-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "canonical current stop completes through fake provider");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->content_type == resume_after_wake_decision_content_type
                   && done->result->output.find("\"decision\":\"stop\"")
                        != std::string::npos,
               "current stop proposal is canonical and durable");
        expect(harness.provider.last_request
                   && harness.provider.last_request->input == exact_input
                   && harness.provider.last_request->content_type
                        == "text/plain; charset=utf-8",
               "fake provider observes exact durable current cognition prompt bytes");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "current stop has one confirmed provider Action");
        const auto budget = harness.budgets.snapshot(
            "provider.call:fake-current", harness.budget_now, budget_policy());
        expect(budget.total_used == 1 && harness.provider.calls == 1,
               "current stop consumes exactly one provider permit/call");
        expect(harness.executor.execute(
                   task_id, "fake-current-worker-reopen", harness.cognition_handler)
                   == ExecuteResult::not_startable
                   && harness.provider.calls == 1,
               "terminal current cognition is not replayed on reopen");
        expect_no_wake(database, "stop");
    }

    {
        TemporaryDatabase database("continue");
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "continue");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain",
            Json{{"schema", resume_after_wake_decision_schema},
                 {"decision", "continue"},
                 {"reason", "Fresh evidence supports one new continuity step."},
                 {"objective", "Record a new current-context snapshot and evaluate the next cognition cycle."}}
                .dump(),
            {}, {}};
        expect(harness.executor.execute(
                   task_id, "fake-current-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "canonical current continue completes through fake provider");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::succeeded && done->result
                   && done->result->output.find("\"decision\":\"continue\"")
                        != std::string::npos
                   && done->result->output.find("\"objective\":")
                        != std::string::npos,
               "current continue proposal is normalized and durable");
        expect_no_wake(database, "continue");
    }
}

void test_invalid_outputs_fail_closed_after_one_confirmed_call()
{
    const std::vector<std::string> invalid_outputs = {
        "not-json",
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\",\"decision\":\"stop\",\"reason\":\"one\",\"reason\":\"two\"}",
        Json{{"schema", resume_after_wake_decision_schema},
             {"decision", "continue"}, {"reason", "missing objective"}}.dump(),
        Json{{"schema", resume_after_wake_decision_schema},
             {"decision", "act"}, {"reason", "unsupported authority"}}.dump(),
        Json{{"schema", resume_after_wake_decision_schema},
             {"decision", "stop"}, {"reason", "semantic mismatch"},
             {"objective", "stop may not carry a string objective"}}.dump(),
        Json{{"schema", resume_after_wake_decision_schema},
             {"decision", "stop"}, {"reason", std::string(1025, 'x')}}.dump()
    };

    for (std::size_t index = 0; index < invalid_outputs.size(); ++index) {
        TemporaryDatabase database("invalid-" + std::to_string(index));
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "invalid-" + std::to_string(index));
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain", invalid_outputs[index], {}, {}};
        expect(harness.executor.execute(
                   task_id, "fake-current-worker", harness.cognition_handler)
                   == ExecuteResult::completed,
               "invalid current provider output closes Task lifecycle");
        const auto done = harness.tasks.find(task_id);
        expect(done && done->status == TaskStatus::failed && done->result
                   && done->result->failure_code == "cognition_invalid_resume_decision",
               "invalid current cognition proposal fails closed");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed
                   && harness.provider.calls == 1,
               "invalid proposal preserves exactly one confirmed provider effect");
        expect_no_wake(database, "invalid output");
    }
}

void test_noncanonical_task_never_crosses_provider_boundary()
{
    TemporaryDatabase database("noncanonical");
    Harness harness(database.path);
    const auto task_id = prepare_current(harness, "noncanonical");
    const auto canonical = harness.tasks.find(task_id);
    if (!canonical) throw std::runtime_error("noncanonical fixture lacks Task");
    auto corrupted = *canonical;
    corrupted.input += "\ncorruption";
    const TaskContext context{corrupted, [] { return false; }};
    const auto result = harness.cognition_handler.execute(context);
    expect(result.outcome == HandlerOutcome::failed
               && result.failure_code == "cognition_invalid_current_task"
               && harness.provider.calls == 0,
           "noncanonical current Task is rejected before provider invocation");
    expect(count_rows(database.path, "actions") == 0,
           "noncanonical current Task creates no Action");
    expect(count_rows(database.path, "budget_consumptions") == 0,
           "noncanonical current Task consumes no budget");
    expect_no_wake(database, "noncanonical");
}

void test_effect_unknown_exception_and_confirmed_crash_never_replay()
{
    {
        TemporaryDatabase database("unknown");
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "unknown");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("unknown fixture lacks Task");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::effect_unknown, {}, {},
            "fake_current_unknown", "synthetic current transport ambiguity"};
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::manual_review
                   && first.failure_code == "fake_current_unknown"
                   && second.outcome == HandlerOutcome::manual_review
                   && harness.provider.calls == 1,
               "ambiguous current provider effect is never replayed");
        expect_no_wake(database, "unknown");
    }

    {
        TemporaryDatabase database("exception");
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "exception");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("exception fixture lacks Task");
        harness.provider.throw_exception = true;
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        harness.provider.throw_exception = false;
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain",
            Json{{"schema", resume_after_wake_decision_schema},
                 {"decision", "stop"}, {"reason", "must not replay"}}.dump(),
            {}, {}};
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::manual_review
                   && first.failure_code == "provider_exception"
                   && second.outcome == HandlerOutcome::manual_review
                   && harness.provider.calls == 1,
               "current provider exception leaves no automatic retry path");
        expect_no_wake(database, "exception");
    }

    {
        TemporaryDatabase database("confirmed-crash");
        Harness harness(database.path);
        const auto task_id = prepare_current(harness, "confirmed-crash");
        const auto task = harness.tasks.find(task_id);
        if (!task) throw std::runtime_error("confirmed crash fixture lacks Task");
        harness.provider.next_result = ProviderResult{
            ProviderOutcome::succeeded, "text/plain",
            Json{{"schema", resume_after_wake_decision_schema},
                 {"decision", "stop"},
                 {"reason", "definite current response before synthetic crash"}}.dump(),
            {}, {}};
        const TaskContext context{*task, [] { return false; }};
        const auto first = harness.cognition_handler.execute(context);
        const auto second = harness.cognition_handler.execute(context);
        expect(first.outcome == HandlerOutcome::succeeded
                   && second.outcome == HandlerOutcome::manual_review
                   && second.failure_code == "provider_response_not_durable"
                   && harness.provider.calls == 1,
               "confirmed current response crash window never recalls provider");
        const auto action = harness.actions.find(action_id(task_id));
        expect(action && action->status == ActionStatus::succeeded
                   && action->effect_result == EffectResult::confirmed,
               "confirmed current Action survives synthetic Task-result crash");
        const auto budget = harness.budgets.snapshot(
            "provider.call:fake-current", harness.budget_now, budget_policy());
        expect(budget.total_used == 1,
               "confirmed current crash consumes exactly one durable permit");
        expect_no_wake(database, "confirmed crash");
    }
}

} // namespace

int main()
{
    test_stop_and_continue_are_durable_and_input_is_exact();
    test_invalid_outputs_fail_closed_after_one_confirmed_call();
    test_noncanonical_task_never_crosses_provider_boundary();
    test_effect_unknown_exception_and_confirmed_crash_never_replay();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All current cognition fake-provider integration tests passed\n";
    return 0;
}
