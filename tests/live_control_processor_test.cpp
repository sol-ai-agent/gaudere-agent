#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "LiveControlProcessor.hpp"
#include "LocalGooseDialogue.hpp"
#include "LocalGooseDialogueV2.hpp"
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
            const bool wake_enabled = false,
            std::string local_goose_dialogue_model_sha256 = {})
        : store(path.string()),
          budget_store(path.string()),
          wake_store(path.string()),
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          wake_runtime(wake_store, [] { return std::chrono::system_clock::now(); },
                       explicit_wake_scope, {explicit_wake_max_total}),
          explicit_wake(store, wake_runtime),
          processor(runtime, store, budget_store,
                    OpenAIActivation::bootstrap_budget_policy(), openai_enabled,
                    wake_enabled ? &explicit_wake : nullptr, {}, {},
                    std::move(local_goose_dialogue_model_sha256))
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

void test_local_goose_dialogue_requires_explicit_capability()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-disabled", "Bonjour Gaudere."});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "disabled local dialogue creates no dispatchable work");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "local dialogue is rejected until a fixed model hash is configured");
}

void test_local_goose_dialogue_submission_is_durable_and_idempotent()
{
    TemporaryDatabase database;
    const std::string model_sha(64, 'a');
    Harness harness(database.path, false, false, model_sha);

    const auto expected = make_local_goose_dialogue_task(
        "dialogue-live-001", "Bonjour Gaudere.", model_sha);

    auto first = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-live-001", "Bonjour Gaudere."});
    const auto first_processed = harness.processor.process(harness.mailbox);
    const auto first_reply = first->wait();
    const auto stored = harness.store.find(expected.id);

    expect(first_processed.work_may_be_pending && first_reply.ok,
           "local dialogue submission creates pending provider-free work");
    expect(stored && stored->kind == local_goose_dialogue_task_kind,
           "local dialogue command persists canonical dialogue Task");
    expect(stored && stored->idempotency_key == expected.idempotency_key,
           "local dialogue request identity is durable and single-use");
    expect(first_reply.body.find(expected.id) != std::string::npos,
           "local dialogue acknowledgement returns durable Task identity");

    auto retry = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-live-001", "Bonjour Gaudere."});
    const auto retry_processed = harness.processor.process(harness.mailbox);
    const auto retry_reply = retry->wait();

    expect(retry_processed.work_may_be_pending && retry_reply.ok,
           "exact local dialogue retry is idempotently acknowledged");
    expect(retry_reply.body.find(expected.id) != std::string::npos,
           "exact retry refers to the original durable dialogue Task");

    const auto conflicting = make_local_goose_dialogue_task(
        "dialogue-live-001", "Message différent.", model_sha);
    auto conflict = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-live-001", "Message différent."});
    const auto conflict_processed = harness.processor.process(harness.mailbox);
    const auto conflict_reply = conflict->wait();

    expect(!conflict_processed.work_may_be_pending
               && !conflict_reply.ok && conflict_reply.code == 4
               && conflict_reply.body.find("conflict") != std::string::npos,
           "reusing a dialogue request id with different bytes fails closed");
    expect(!harness.store.find(conflicting.id),
           "conflicting dialogue retry creates no second Task");
}

gaudere::work::Task succeeded_v2_root(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha,
    const std::string& response)
{
    auto task = make_local_goose_dialogue_v2_root_task(
        request_id, message, model_sha);
    const auto inspection = inspect_local_goose_dialogue_v2_task(task);
    task.status = gaudere::work::TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = gaudere::work::TaskResult{
        local_goose_dialogue_v2_response_content_type,
        make_local_goose_dialogue_v2_response(inspection, response),
        {},
        {}};
    return task;
}

void test_local_goose_dialogue_v2_submission_is_durable_and_linked()
{
    TemporaryDatabase database;
    const std::string model_sha(64, 'a');
    Harness harness(database.path, false, false, model_sha);

    const auto expected_root = make_local_goose_dialogue_v2_root_task(
        "v2-live-root", "Bonjour V2.", model_sha);
    auto root_request = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_root,
            "v2-live-root", "Bonjour V2."});
    const auto root_processed = harness.processor.process(harness.mailbox);
    const auto root_reply = root_request->wait();
    const auto stored_root = harness.store.find(expected_root.id);

    expect(root_processed.work_may_be_pending && root_reply.ok,
           "V2 root submission creates pending provider-free work");
    expect(stored_root && stored_root->kind == local_goose_dialogue_v2_task_kind,
           "V2 root command persists canonical V2 Task");
    expect(root_reply.body.find(expected_root.id) != std::string::npos,
           "V2 root acknowledgement returns durable Task identity");

    auto root_conflict = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_root,
            "v2-live-root", "Message différent."});
    const auto root_conflict_processed =
        harness.processor.process(harness.mailbox);
    const auto root_conflict_reply = root_conflict->wait();
    expect(!root_conflict_processed.work_may_be_pending
               && !root_conflict_reply.ok
               && root_conflict_reply.code == 4
               && root_conflict_reply.body.find("conflict") != std::string::npos,
           "V2 root request id is single-use across different message bytes");

    const auto canonical_root = succeeded_v2_root(
        "v2-predecessor-root", "Racine.", model_sha, "Réponse racine.");
    harness.store.save(canonical_root);

    auto missing = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "v2-missing", "Suite.",
            "cognition.local-goose-dialogue.v2:" + std::string(64, 'f')});
    const auto missing_processed = harness.processor.process(harness.mailbox);
    const auto missing_reply = missing->wait();
    expect(!missing_processed.work_may_be_pending
               && !missing_reply.ok && missing_reply.code == 3
               && missing_reply.body.find("not found") != std::string::npos,
           "V2 successor rejects a missing durable predecessor");

    const auto pending_root = make_local_goose_dialogue_v2_root_task(
        "v2-pending-root", "En attente.", model_sha);
    harness.store.save(pending_root);
    auto ineligible = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "v2-ineligible", "Suite.", pending_root.id});
    const auto ineligible_processed =
        harness.processor.process(harness.mailbox);
    const auto ineligible_reply = ineligible->wait();
    expect(!ineligible_processed.work_may_be_pending
               && !ineligible_reply.ok && ineligible_reply.code == 4
               && ineligible_reply.body.find("predecessor rejected")
                    != std::string::npos,
           "V2 successor requires canonical successful predecessor evidence");

    const auto expected_next = make_local_goose_dialogue_v2_successor_task(
        "v2-live-next", "Suite liée.", model_sha, canonical_root);
    auto next = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "v2-live-next", "Suite liée.", canonical_root.id});
    const auto next_processed = harness.processor.process(harness.mailbox);
    const auto next_reply = next->wait();
    const auto stored_next = harness.store.find(expected_next.id);
    expect(next_processed.work_may_be_pending && next_reply.ok,
           "V2 successor submission creates pending linked work");
    expect(stored_next
               && stored_next->kind == local_goose_dialogue_v2_task_kind,
           "V2 successor is persisted with V2 Task kind");
    expect(stored_next
               && inspect_local_goose_dialogue_v2_task(*stored_next).eligible
               && inspect_local_goose_dialogue_v2_task(*stored_next)
                      .predecessor_task_id == canonical_root.id,
           "V2 successor persists canonical predecessor identity");

    auto retry = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "v2-live-next", "Suite liée.", canonical_root.id});
    const auto retry_processed = harness.processor.process(harness.mailbox);
    const auto retry_reply = retry->wait();
    expect(retry_processed.work_may_be_pending && retry_reply.ok
               && retry_reply.body.find(expected_next.id) != std::string::npos,
           "exact V2 successor retry is idempotently acknowledged");

    auto successor_conflict = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "v2-live-next", "Autre suite.", canonical_root.id});
    const auto successor_conflict_processed =
        harness.processor.process(harness.mailbox);
    const auto successor_conflict_reply = successor_conflict->wait();
    expect(!successor_conflict_processed.work_may_be_pending
               && !successor_conflict_reply.ok
               && successor_conflict_reply.code == 4
               && successor_conflict_reply.body.find("conflict")
                    != std::string::npos,
           "V2 successor request id conflicts on changed message bytes");
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

void test_local_goose_stimulus_requires_explicit_capability()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::stimulate_local_goose_cycle,
            "recheck-disabled", {}});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1
               && !processed.local_goose_cycle_may_have_changed,
           "disabled Local Goose stimulus changes no cycle scheduling state");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "Local Goose stimulus is rejected until a bounded worker handler is installed");
}

void test_local_goose_stimulus_runs_only_on_worker_callback()
{
    TemporaryDatabase database;
    gaudere::persistence::sqlite::TaskStore store(database.path.string());
    gaudere::persistence::sqlite::BudgetStore budget_store(
        database.path.string());
    gaudere::work::Runtime runtime(
        store, [] { return std::chrono::system_clock::now(); });
    runtime.recover();

    std::string observed_request;
    LiveControlProcessor processor(
        runtime, store, budget_store,
        OpenAIActivation::bootstrap_budget_policy(), false, nullptr, {},
        [&](const std::string& request_id) {
            observed_request = request_id;
            return LiveControlReply{true, 0, "result=consumed\n"};
        });
    LiveControlMailbox mailbox;

    auto pending = mailbox.submit(
        LiveControlCommand{
            LiveControlOperation::stimulate_local_goose_cycle,
            "recheck-worker-001", {}});
    const auto processed = processor.process(mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1
               && processed.local_goose_cycle_may_have_changed
               && !processed.work_may_be_pending
               && !processed.wake_deadline_may_have_changed,
           "stimulus callback requests conservative cycle refresh only");
    expect(observed_request == "recheck-worker-001",
           "sole worker callback receives exactly the bounded request identity");
    expect(reply.ok && reply.code == 0
               && reply.body == "result=consumed\n",
           "worker callback owns the complete stimulus reply");
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
    test_local_goose_dialogue_requires_explicit_capability();
    test_local_goose_dialogue_submission_is_durable_and_idempotent();
    test_local_goose_dialogue_v2_submission_is_durable_and_linked();
    test_inspect_reads_durable_task_without_submission();
    test_budget_status_is_observational_and_live();
    test_duplicate_preserves_original_definition();
    test_local_goose_stimulus_requires_explicit_capability();
    test_local_goose_stimulus_runs_only_on_worker_callback();
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
