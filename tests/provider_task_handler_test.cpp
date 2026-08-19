#include "ProviderTaskHandler.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using ActionRuntime = gaudere::scheduling::wake::Runtime;
using ActionStatus = gaudere::scheduling::wake::ActionStatus;
using EffectResult = gaudere::scheduling::wake::EffectResult;
using WorkRuntime = gaudere::work::Runtime;
using TaskStatus = gaudere::work::TaskStatus;
using namespace std::chrono_literals;
using namespace gaudere_agent;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-provider-handler-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
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

gaudere::work::Task make_task(std::string id)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "task-key:" + task.id;
    task.kind = "provider.fake";
    task.input_content_type = "text/plain";
    task.input = "hello provider";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 2048;
    task.limits.max_runtime = 2s;
    task.limits.max_attempts = 2;
    return task;
}

class FakeProvider final : public Provider {
public:
    enum class Mode {
        success,
        rejected,
        unknown,
        throws
    };

    explicit FakeProvider(const Mode mode) : mode_(mode) {}

    std::string_view name() const noexcept override { return "fake"; }

    ProviderResult invoke(const ProviderRequest& request) override
    {
        ++calls;
        last_request = request;
        switch (mode_) {
        case Mode::success:
            return ProviderResult{ProviderOutcome::succeeded,
                                  "text/plain", "provider answer", {}, {}};
        case Mode::rejected:
            return ProviderResult{ProviderOutcome::rejected,
                                  {}, {}, "fake_rejected", "definite rejection"};
        case Mode::unknown:
            return ProviderResult{ProviderOutcome::effect_unknown,
                                  {}, {}, "fake_unknown", "transport result ambiguous"};
        case Mode::throws:
            throw std::runtime_error("fake transport exception");
        }
        throw std::runtime_error("invalid fake provider mode");
    }

    int calls = 0;
    std::optional<ProviderRequest> last_request;

private:
    Mode mode_;
};

struct Harness {
    explicit Harness(const std::filesystem::path& path,
                     FakeProvider::Mode mode,
                     gaudere::scheduling::wake::TimePoint now = {})
        : action_store(path.string()),
          task_store(path.string()),
          action_runtime(action_store, [now] { return now; }),
          work_runtime(task_store, [now] { return now; }),
          provider(mode),
          handler(action_runtime, action_store, provider),
          executor(work_runtime, task_store)
    {
        action_runtime.recover();
        work_runtime.recover();
    }

    gaudere::persistence::sqlite::ActionStore action_store;
    gaudere::persistence::sqlite::TaskStore task_store;
    ActionRuntime action_runtime;
    WorkRuntime work_runtime;
    FakeProvider provider;
    ProviderTaskHandler handler;
    TaskExecutor executor;
};

void test_success()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::success);
    const auto task = make_task("success");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "success task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "success provider task completes");

    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::succeeded && done->result
               && done->result->output == "provider answer",
           "provider success becomes a durable task result");
    const auto action = harness.action_store.find("provider.call:fake:success");
    expect(action && action->status == ActionStatus::succeeded
               && action->effect_result == EffectResult::confirmed && !action->lease,
           "provider success confirms the recoverable external action");
    expect(harness.provider.calls == 1 && harness.provider.last_request
               && harness.provider.last_request->idempotency_key
                    == "provider.call:fake:task-key:success"
               && harness.provider.last_request->max_output_bytes == 2048
               && harness.provider.last_request->max_runtime == 2s,
           "provider receives bounded deterministic request metadata");
}

void test_definite_rejection()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::rejected);
    const auto task = make_task("rejected");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "rejected task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "definite rejection completes the task lifecycle");

    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::failed && done->result
               && done->result->failure_code == "fake_rejected",
           "definite provider rejection is a durable task failure");
    const auto action = harness.action_store.find("provider.call:fake:rejected");
    expect(action && action->status == ActionStatus::succeeded
               && action->effect_result == EffectResult::confirmed,
           "definite rejection still confirms that the external call completed");
}

void test_ambiguous_result()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::unknown);
    const auto task = make_task("unknown");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "ambiguous task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "ambiguous provider result completes as manual review");

    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::manual_review && done->result
               && done->result->failure_code == "fake_unknown",
           "ambiguous provider result is durable manual review");
    const auto action = harness.action_store.find("provider.call:fake:unknown");
    expect(action && action->status == ActionStatus::manual_review
               && action->effect_result == EffectResult::unknown,
           "ambiguous external action is never retryable");
}

void test_provider_exception()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::throws);
    const auto task = make_task("exception");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "exception task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "provider exception is contained by the handler");

    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::manual_review && done->result
               && done->result->failure_code == "provider_exception",
           "provider exception becomes task manual review");
    const auto action = harness.action_store.find("provider.call:fake:exception");
    expect(action && action->status == ActionStatus::manual_review
               && action->effect_result == EffectResult::unknown,
           "provider exception records unknown external effect immediately");
}

void test_pre_call_cancellation()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::success);
    const auto task = make_task("cancelled");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "pre-call cancellation task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler,
                                    [] { return true; })
               == ExecuteResult::completed,
           "pre-call worker stop is acknowledged");

    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::cancelled,
           "task is durably cancelled before provider invocation");
    expect(harness.provider.calls == 0,
           "pre-call cancellation never invokes the provider");
    expect(!harness.action_store.find_by_idempotency_key(
               "provider.call:fake:task-key:cancelled"),
           "pre-call cancellation creates no external action");
}

void test_existing_manual_review_is_not_replayed()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::success);
    const auto task = make_task("existing-unknown");

    gaudere::scheduling::wake::Action action;
    action.id = "provider.call:fake:" + task.id;
    action.idempotency_key = "provider.call:fake:" + task.idempotency_key;
    action.critical = true;
    expect(harness.action_runtime.submit(action)
               == gaudere::scheduling::wake::SubmitResult::accepted,
           "existing action is submitted");
    expect(harness.action_runtime.start(action.id, "old-worker", 1s),
           "existing action starts");
    expect(harness.action_runtime.record_effect_started(action.id),
           "existing action crosses the effect marker");
    expect(harness.action_runtime.record_unknown_result(action.id),
           "existing action is already manual review");

    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "retrying task is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "retrying task sees existing manual-review action");
    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::manual_review && done->result
               && done->result->failure_code == "provider_effect_unknown",
           "existing uncertain action propagates task manual review");
    expect(harness.provider.calls == 0,
           "existing uncertain action is never replayed");
}

void test_confirmed_call_without_task_receipt_is_not_replayed()
{
    TemporaryDatabase database;
    Harness harness(database.path, FakeProvider::Mode::success);
    const auto task = make_task("lost-response");

    gaudere::scheduling::wake::Action action;
    action.id = "provider.call:fake:" + task.id;
    action.idempotency_key = "provider.call:fake:" + task.idempotency_key;
    action.critical = true;
    expect(harness.action_runtime.submit(action)
               == gaudere::scheduling::wake::SubmitResult::accepted,
           "confirmed action is submitted");
    expect(harness.action_runtime.start(action.id, "old-worker", 1s),
           "confirmed action starts");
    expect(harness.action_runtime.record_effect_started(action.id),
           "confirmed action crosses effect marker");
    expect(harness.action_runtime.record_confirmed_result(action.id),
           "provider call is durably confirmed without a task receipt");

    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "task with lost provider response is submitted");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "lost-response task completes conservatively");
    const auto done = harness.task_store.find(task.id);
    expect(done && done->status == TaskStatus::manual_review && done->result
               && done->result->failure_code == "provider_response_not_durable",
           "confirmed call without durable response requires manual review");
    expect(harness.provider.calls == 0,
           "confirmed provider call is never repeated to reconstruct a lost response");
}

void test_crash_after_effect_start_never_replays()
{
    TemporaryDatabase database;
    const auto first_time = gaudere::scheduling::wake::TimePoint{};
    {
        gaudere::persistence::sqlite::ActionStore action_store(database.path.string());
        ActionRuntime runtime(action_store, [first_time] { return first_time; });
        runtime.recover();
        gaudere::scheduling::wake::Action action;
        action.id = "provider.call:fake:crash";
        action.idempotency_key = "provider.call:fake:task-key:crash";
        action.critical = true;
        expect(runtime.submit(action) == gaudere::scheduling::wake::SubmitResult::accepted,
               "crash action is submitted");
        expect(runtime.start(action.id, "dead-worker", 1ms),
               "crash action starts");
        expect(runtime.record_effect_started(action.id),
               "effect-start marker is durable before simulated process death");
    }

    const auto recovery_time = first_time + 2ms;
    Harness harness(database.path, FakeProvider::Mode::success, recovery_time);
    const auto recovered_action = harness.action_store.find("provider.call:fake:crash");
    expect(recovered_action && recovered_action->status == ActionStatus::manual_review
               && recovered_action->effect_result == EffectResult::unknown,
           "replacement runtime recovers interrupted external effect to manual review");

    const auto task = make_task("crash");
    expect(harness.work_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "replacement task is submitted after simulated crash");
    expect(harness.executor.execute(task.id, "worker", harness.handler)
               == ExecuteResult::completed,
           "replacement task observes the recovered action");
    expect(harness.task_store.find(task.id)->status == TaskStatus::manual_review,
           "replacement task requires manual review");
    expect(harness.provider.calls == 0,
           "replacement process does not duplicate the provider call");
}

} // namespace

int main()
{
    test_success();
    test_definite_rejection();
    test_ambiguous_result();
    test_provider_exception();
    test_pre_call_cancellation();
    test_existing_manual_review_is_not_replayed();
    test_confirmed_call_without_task_receipt_is_not_replayed();
    test_crash_after_effect_start_never_replays();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All provider task handler tests passed\n";
    return 0;
}
