#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

struct TemporaryFile {
    explicit TemporaryFile(std::string stem)
    {
        path = std::filesystem::temp_directory_path()
            / (std::move(stem) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch().count())
               + ".db");
    }

    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

std::string model_sha()
{
    return std::string(64, 'a');
}

Task succeed_v2(Task task, const std::string& response)
{
    const auto inspected = inspect_local_goose_dialogue_v2_task(task);
    assert(inspected.eligible);
    task.status = TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = TaskResult{
        local_goose_dialogue_v2_response_content_type,
        make_local_goose_dialogue_v2_response(inspected, response),
        {},
        {}};
    assert(canonical_local_goose_dialogue_v2_success(task));
    return task;
}

void complete_v3(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& store,
    const std::string& task_id,
    const std::string& response)
{
    const auto task = store.find(task_id);
    assert(task);
    const auto inspected = inspect_local_goose_dialogue_v3_task(*task);
    assert(inspected.eligible);
    assert(runtime.start(task_id, "responder-test"));
    const auto output =
        make_local_goose_dialogue_v3_response(inspected, response);
    assert(runtime.succeed(
        task_id, output, local_goose_dialogue_v3_response_content_type)
        == gaudere::work::FinishResult::accepted);
    const auto completed = store.find(task_id);
    assert(completed && canonical_local_goose_dialogue_v3_success(*completed));
}

LocalGooseDialogueResponderLease make_lease()
{
    LocalGooseDialogueResponderLease lease;
    lease.lease_id = "lease-responder-test";
    lease.thread_alias = "main";
    lease.speaker_id = "sol";
    lease.allowed_message_kinds =
        local_goose_dialogue_responder_message_feedback
        | local_goose_dialogue_responder_message_intervention;
    lease.purpose = "Bounded responder dispatcher integration proof";
    lease.max_system_turns = 4;
    lease.issued_at_ms = 1000;
    lease.expires_at_ms = 100000;
    lease.min_interval_ms = 0;
    lease.next_eligible_at_ms = lease.issued_at_ms;
    return lease;
}

} // namespace

int main()
{
    TemporaryFile core{"gaudere-responder-dispatch-core"};
    TemporaryFile thread{"gaudere-responder-dispatch-thread"};
    TemporaryFile feed{"gaudere-responder-dispatch-feed"};
    TemporaryFile responder{"gaudere-responder-dispatch-state"};

    gaudere::persistence::sqlite::TaskStore task_store(core.path.string());
    gaudere::work::Runtime runtime(
        task_store, [] { return std::chrono::system_clock::now(); });
    runtime.recover();

    LocalGooseDialogueThreadStore thread_store(thread.path.string());
    LocalGooseDialogueCompletionFeedStore completion_store(feed.path.string());
    LocalGooseDialogueResponderStore responder_store(responder.path.string());

    const auto model = model_sha();
    const auto root = succeed_v2(
        make_local_goose_dialogue_v2_root_task(
            "responder-root", "Bonjour.", model),
        "Bonjour.");
    const auto initial_head_task = succeed_v2(
        make_local_goose_dialogue_v2_successor_task(
            "responder-head", "Continue.", model, root),
        "Je continue.");

    task_store.save(root);
    task_store.save(initial_head_task);

    const LocalGooseDialogueThreadHead initial_head{
        "main", 0, root.id, initial_head_task.id};
    assert(thread_store.seed(initial_head).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto first_event = make_local_goose_dialogue_completion_event(
        "main", 0, initial_head_task, 1000);
    const auto first_append = completion_store.append_for_revision(first_event);
    assert(first_append.result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(first_append.event && first_append.event->sequence == 1);

    const auto lease = make_lease();
    assert(responder_store.create_lease(lease).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    std::int64_t now = 2000;
    LocalGooseDialogueResponderDispatcher dispatcher(
        runtime, task_store, thread_store, completion_store, responder_store,
        model, "responder-test", [&now] { return now; });

    const auto prepared1 = dispatcher.prepare(
        lease.lease_id, 1, "feedback", "Première intervention.", 5000);
    assert(prepared1.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared1.intent);
    assert(prepared1.intent->state
        == LocalGooseDialogueResponderIntentState::prepared);
    assert(prepared1.consumer_last_sequence == 0);
    const auto head_before_dispatch = thread_store.find("main");
    assert(head_before_dispatch && head_before_dispatch->revision == 0);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{0});

    const auto dispatched1 = dispatcher.dispatch(prepared1.intent->intent_id);
    assert(dispatched1.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(dispatched1.intent
        && dispatched1.intent->state
            == LocalGooseDialogueResponderIntentState::completed);
    assert(dispatched1.task && dispatched1.head);
    assert(dispatched1.head->revision == 1);
    assert(dispatched1.head->head_task_id == dispatched1.task->id);
    assert(dispatched1.consumer_last_sequence == 1);
    const auto lease_after1 = responder_store.find_lease(lease.lease_id);
    assert(lease_after1 && lease_after1->turns_committed == 1);

    const auto replay1 = dispatcher.dispatch(prepared1.intent->intent_id);
    assert(replay1.result
        == LocalGooseDialogueResponderDispatchCode::duplicate);
    const auto lease_after_replay = responder_store.find_lease(lease.lease_id);
    assert(lease_after_replay && lease_after_replay->turns_committed == 1);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{1});

    complete_v3(
        runtime, task_store, dispatched1.task->id,
        "Réponse à la première intervention.");
    const auto completed1 = task_store.find(dispatched1.task->id);
    assert(completed1);
    const auto second_event = make_local_goose_dialogue_completion_event(
        "main", 1, *completed1, 2100);
    const auto second_append = completion_store.append_for_revision(second_event);
    assert(second_append.result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(second_append.event && second_append.event->sequence == 2);

    now = 2200;
    const auto prepared2 = dispatcher.prepare(
        lease.lease_id, 2, "intervention",
        "Intervention à reprendre après crash.", 5000);
    assert(prepared2.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared2.intent);

    const auto submitted_outside_accounting =
        submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model,
            prepared2.intent->request_id,
            prepared2.intent->thread_alias,
            prepared2.intent->expected_thread_revision,
            prepared2.intent->speaker_kind,
            prepared2.intent->speaker_id,
            prepared2.intent->message_kind,
            prepared2.intent->message);
    assert(submitted_outside_accounting.result
        == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(submitted_outside_accounting.task
        && submitted_outside_accounting.head
        && submitted_outside_accounting.head->revision == 2);
    const auto still_prepared =
        responder_store.find_intent(prepared2.intent->intent_id);
    assert(still_prepared
        && still_prepared->state
            == LocalGooseDialogueResponderIntentState::prepared);
    assert(responder_store.find_lease(lease.lease_id)->turns_committed == 1);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{1});

    now = 2300;
    const auto recovered2 = dispatcher.dispatch(prepared2.intent->intent_id);
    assert(recovered2.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(recovered2.intent
        && recovered2.intent->state
            == LocalGooseDialogueResponderIntentState::completed);
    assert(recovered2.consumer_last_sequence == 2);
    const auto lease_after2 = responder_store.find_lease(lease.lease_id);
    assert(lease_after2 && lease_after2->turns_committed == 2);
    assert(thread_store.find("main")->revision == 2);

    const auto replay2 = dispatcher.dispatch(prepared2.intent->intent_id);
    assert(replay2.result
        == LocalGooseDialogueResponderDispatchCode::duplicate);
    assert(responder_store.find_lease(lease.lease_id)->turns_committed == 2);

    complete_v3(
        runtime, task_store, recovered2.task->id,
        "Réponse à la seconde intervention.");
    const auto completed2 = task_store.find(recovered2.task->id);
    assert(completed2);
    const auto third_event = make_local_goose_dialogue_completion_event(
        "main", 2, *completed2, 2400);
    const auto third_append = completion_store.append_for_revision(third_event);
    assert(third_append.result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(third_append.event && third_append.event->sequence == 3);

    now = 2500;
    const auto prepared3 = dispatcher.prepare(
        lease.lease_id, 3, "feedback",
        "Cette intervention doit devenir stale.", 5000);
    assert(prepared3.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared3.intent);

    const auto competing = submit_local_goose_dialogue_v3_preferred(
        runtime, task_store, thread_store, model,
        "competing-human-turn", "main", 2,
        "human", "bertrand", "dialogue", "Le tour humain gagne.");
    assert(competing.result
        == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(competing.head && competing.head->revision == 3);

    now = 2600;
    const auto stale3 = dispatcher.dispatch(prepared3.intent->intent_id);
    assert(stale3.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    const auto terminal3 =
        responder_store.find_intent(prepared3.intent->intent_id);
    assert(terminal3
        && terminal3->state
            == LocalGooseDialogueResponderIntentState::conflict);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{2});
    assert(responder_store.find_lease(lease.lease_id)->turns_committed == 2);
    assert(thread_store.find("main")->head_task_id == competing.task->id);

    now = 100001;
    const auto replacement_lease = dispatcher.create_lease(
        "lease-responder-replacement", "main", "sol", "feedback",
        "Replacement after finite expiry", 1, 5000, 0);
    assert(replacement_lease.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(replacement_lease.lease
        && replacement_lease.lease->state
            == LocalGooseDialogueResponderLeaseState::active);
    const auto expired_original =
        responder_store.find_lease(lease.lease_id);
    assert(expired_original
        && expired_original->state
            == LocalGooseDialogueResponderLeaseState::expired);
    assert(expired_original->terminal_reason == "lease_expired");

    const auto identity_a = make_local_goose_dialogue_responder_intent(
        "identity-lease", "main", 7,
        std::string{local_goose_dialogue_completion_event_prefix}
            + std::string(64, '1'),
        std::string(64, '2'),
        std::string{local_goose_dialogue_v3_task_prefix}
            + std::string(64, '3'),
        6,
        std::string{local_goose_dialogue_v3_task_prefix}
            + std::string(64, '3'),
        "sol", "feedback", "Identité stable.", 100, 200);
    const auto identity_b = make_local_goose_dialogue_responder_intent(
        "identity-lease", "main", 7,
        std::string{local_goose_dialogue_completion_event_prefix}
            + std::string(64, '1'),
        std::string(64, '2'),
        std::string{local_goose_dialogue_v3_task_prefix}
            + std::string(64, '3'),
        6,
        std::string{local_goose_dialogue_v3_task_prefix}
            + std::string(64, '3'),
        "sol", "feedback", "Identité stable.", 101, 201);
    assert(identity_a.intent_id == identity_b.intent_id);
    assert(identity_a.request_id == identity_b.request_id);

    std::cout << "local_goose_dialogue_responder_dispatcher_test: PASS\n";
    return 0;
}
