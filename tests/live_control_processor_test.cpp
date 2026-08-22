#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "LiveControlProcessor.hpp"
#include "OpenAIActivation.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

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
            / ("gaudere-live-control-processor-test-" + std::to_string(
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

struct Harness {
    Harness(const std::filesystem::path& path,
            const bool openai_enabled,
            const bool wake_enabled = false)
        : store(path.string()),
          budget_store(path.string()),
          wake_store(path.string()),
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          wake_runtime(wake_store, [] { return std::chrono::system_clock::now(); },
                       explicit_wake_scope, {explicit_wake_max_total}),
          explicit_wake(store, wake_runtime),
          processor(runtime, store, budget_store,
                    OpenAIActivation::bootstrap_budget_policy(), openai_enabled,
                    wake_enabled ? &explicit_wake : nullptr)
    {
        runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore store;
    gaudere::persistence::sqlite::BudgetStore budget_store;
    gaudere::persistence::sqlite::WakeIntentStore wake_store;
    gaudere::work::Runtime runtime;
    gaudere::scheduling::wake::WakeIntentRuntime wake_runtime;
    ExplicitWake explicit_wake;
    LiveControlProcessor processor;
    LiveControlMailbox mailbox;
};

gaudere::work::Task reflection_source(std::string id, std::string decision)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "live control source fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = std::chrono::seconds{1};
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type, std::move(decision), {}, {}};
    return task;
}

void test_echo_submission_is_durable()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "echo-live", "bonjour"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();
    const auto task = harness.store.find("echo-live");

    expect(processed.processed == 1 && processed.work_may_be_pending,
           "echo submission asks the worker loop to dispatch pending work");
    expect(reply.ok && reply.code == 0,
           "echo submission receives successful durable acknowledgement");
    expect(reply.body.find("status=pending") != std::string::npos,
           "echo reply is a durable pending Task report");
    expect(task && task->kind == "local.echo" && task->input == "bonjour",
           "echo command is persisted through Runtime on worker side");
    expect(task && task->limits.max_attempts == 1,
           "live echo uses the same bounded local task policy");
}

void test_openai_submission_requires_activated_provider()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_openai,
                           "ai-disabled", "bonjour"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "disabled provider does not create dispatchable work");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "OpenAI submission is explicitly rejected when service is offline-provider mode");
    expect(!harness.store.find("ai-disabled"),
           "disabled provider command creates no durable Task");
}

void test_openai_submission_uses_bounded_task_factory()
{
    TemporaryDatabase database;
    Harness harness(database.path, true);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_openai,
                           "ai-live", "réponds brièvement"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();
    const auto task = harness.store.find("ai-live");

    expect(processed.work_may_be_pending && reply.ok,
           "enabled OpenAI command creates pending work");
    expect(task && task->kind == "provider.openai.responses",
           "live OpenAI command uses provider task kind");
    expect(task && task->limits.max_input_bytes == 16 * 1024
               && task->limits.max_output_bytes == 64 * 1024
               && task->limits.max_attempts == 2,
           "live OpenAI command reuses bounded OpenAI Task factory");
}

void test_reflection_submission_requires_activated_provider()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_reflection,
                           "reflect-disabled", "Consider one next step."});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "disabled provider does not create reflection work");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "reflection is rejected when provider capability is disabled");
    expect(!harness.store.find("reflect-disabled"),
           "disabled reflection creates no durable Task");
}

void test_reflection_submission_is_bounded_and_explicit()
{
    TemporaryDatabase database;
    Harness harness(database.path, true);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_reflection,
                           "reflect-live", "Consider one next step."});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();
    const auto task = harness.store.find("reflect-live");

    expect(processed.work_may_be_pending && reply.ok,
           "explicit reflection command creates pending work");
    expect(task && task->kind == bounded_reflection_task_kind
               && task->idempotency_key
                    == "cognition.reflect.v1:reflect-live",
           "reflection command uses distinct deterministic task identity");
    expect(task && task->input.find("Consider one next step.")
                       != std::string::npos
               && task->input.find("proposal only") != std::string::npos,
           "reflection persists fixed prompt and bounded objective");
    expect(task && task->limits.max_input_bytes == 16 * 1024
               && task->limits.max_output_bytes == 4096
               && task->limits.max_attempts == 2,
           "reflection live control applies hard task limits");
}

void test_inspect_reads_durable_task_without_submission()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);

    auto submit = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "inspect-me", "persisted"});
    static_cast<void>(harness.processor.process(harness.mailbox));
    static_cast<void>(submit->wait());

    auto inspect = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_task,
                           "inspect-me", {}});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = inspect->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "inspect does not itself request dispatch");
    expect(reply.ok && reply.body.find("id=\"inspect-me\"") != std::string::npos
               && reply.body.find("status=pending") != std::string::npos,
           "inspect returns the durable Task report");
}

void test_budget_status_is_observational_and_live()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);

    auto empty_request = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_budget, "openai", {}});
    const auto empty_processed = harness.processor.process(harness.mailbox);
    const auto empty = empty_request->wait();

    expect(empty_processed.processed == 1 && !empty_processed.work_may_be_pending,
           "budget inspection never requests task dispatch");
    expect(empty.ok
               && empty.body.find("provider_enabled=false") != std::string::npos
               && empty.body.find("max_total=12") != std::string::npos
               && empty.body.find("total_used=0") != std::string::npos
               && empty.body.find("next_new_call=available") != std::string::npos,
           "empty live budget status reports bootstrap limits and availability");

    const auto now = std::chrono::system_clock::now();
    expect(harness.budget_store.consume(
               std::string(OpenAIActivation::bootstrap_budget_scope()),
               "test-permit", now, OpenAIActivation::bootstrap_budget_policy())
               == gaudere::budget::ConsumeResult::accepted,
           "budget status test consumes one synthetic durable permit");

    auto used_request = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_budget, "openai", {}});
    static_cast<void>(harness.processor.process(harness.mailbox));
    const auto used = used_request->wait();

    expect(used.ok
               && used.body.find("total_used=1") != std::string::npos
               && used.body.find("in_window_used=1") != std::string::npos
               && used.body.find("remaining_total=11") != std::string::npos
               && used.body.find("next_new_call=cooldown") != std::string::npos,
           "live budget status observes durable consumption without spending another permit");
}

void test_duplicate_preserves_original_definition()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);

    auto first = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "same", "first"});
    static_cast<void>(harness.processor.process(harness.mailbox));
    static_cast<void>(first->wait());

    auto second = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "same", "different"});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = second->wait();
    const auto task = harness.store.find("same");

    expect(reply.ok && processed.work_may_be_pending,
           "duplicate pending command is acknowledged and re-wakes dispatcher");
    expect(task && task->input == "first",
           "durable idempotency preserves original task definition");
}

void test_wake_commands_require_explicit_capability_activation()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::accept_wake,
                           "source-disabled", {}});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1
               && !processed.work_may_be_pending
               && !processed.wake_deadline_may_have_changed,
           "disabled wake command changes no worker scheduling state");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "wake command is explicitly rejected while capability flag is absent");
}

void test_stop_source_is_permanently_ineligible()
{
    TemporaryDatabase database;
    Harness harness(database.path, false, true);
    harness.store.save(reflection_source(
        "stop-source",
        "{\"decision\":\"stop\",\"reason\":\"Complete.\","
        "\"schema\":\"gaudere.cognition.decision.v1\"}"));
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::accept_wake,
                           "stop-source", {}});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(!reply.ok && reply.code == 4
               && reply.body.find("source_ineligible") != std::string::npos,
           "durable stop decision cannot be accepted as a wake source");
    expect(!processed.wake_deadline_may_have_changed
               && !harness.explicit_wake.find("stop-source"),
           "ineligible stop result creates no deadline or wake row");
}

void test_accept_inspect_revoke_wake_lifecycle()
{
    TemporaryDatabase database;
    Harness harness(database.path, false, true);
    harness.store.save(reflection_source(
        "wake-source",
        "{\"decision\":\"propose_wake\","
        "\"reason\":\"Revisit once.\","
        "\"schema\":\"gaudere.cognition.decision.v1\","
        "\"wake_after_seconds\":900}"));

    auto accept = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::accept_wake,
                           "wake-source", {}});
    const auto accepted = harness.processor.process(harness.mailbox);
    const auto accepted_reply = accept->wait();
    expect(accepted.wake_deadline_may_have_changed
               && !accepted.work_may_be_pending
               && accepted_reply.ok
               && accepted_reply.body.find("acceptance=accepted")
                    != std::string::npos
               && accepted_reply.body.find("status=scheduled")
                    != std::string::npos,
           "explicit acceptance returns complete durable scheduled report");

    auto duplicate = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::accept_wake,
                           "wake-source", {}});
    static_cast<void>(harness.processor.process(harness.mailbox));
    expect(duplicate->wait().body.find("acceptance=duplicate")
               != std::string::npos,
           "duplicate live acceptance is observable and idempotent");

    auto inspect = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_wake,
                           "wake-source", {}});
    const auto inspected = harness.processor.process(harness.mailbox);
    const auto inspected_reply = inspect->wait();
    expect(!inspected.wake_deadline_may_have_changed
               && inspected_reply.ok
               && inspected_reply.body.find("source_id=\"wake-source\"")
                    != std::string::npos,
           "wake inspection is observational through the sole worker");

    auto revoke = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::revoke_wake,
                           "wake-source", "operator request"});
    const auto revoked = harness.processor.process(harness.mailbox);
    const auto revoked_reply = revoke->wait();
    expect(revoked.wake_deadline_may_have_changed
               && revoked_reply.ok
               && revoked_reply.body.find("revocation=revoked")
                    != std::string::npos
               && revoked_reply.body.find("terminal_reason=\"operator request\"")
                    != std::string::npos,
           "pre-due live revocation is terminal and completely reported");
}

} // namespace

int main()
{
    test_echo_submission_is_durable();
    test_openai_submission_requires_activated_provider();
    test_openai_submission_uses_bounded_task_factory();
    test_reflection_submission_requires_activated_provider();
    test_reflection_submission_is_bounded_and_explicit();
    test_inspect_reads_durable_task_without_submission();
    test_budget_status_is_observational_and_live();
    test_duplicate_preserves_original_definition();
    test_wake_commands_require_explicit_capability_activation();
    test_stop_source_is_permanently_ineligible();
    test_accept_inspect_revoke_wake_lifecycle();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All live control processor tests passed\n";
    return 0;
}
