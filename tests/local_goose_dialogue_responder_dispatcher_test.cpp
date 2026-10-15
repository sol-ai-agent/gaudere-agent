#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"

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

LocalGooseDialogueCompletionEvent append_completed(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    gaudere::work::TaskStore& task_store,
    const std::string& task_id,
    const std::uint64_t revision,
    const std::int64_t observed_at_ms)
{
    const auto task = task_store.find(task_id);
    assert(task);
    const auto event = make_local_goose_dialogue_completion_event(
        "main", revision, *task, observed_at_ms);
    const auto write = completion_store.append_for_revision(event);
    assert(write.result == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(write.event);
    return *write.event;
}

void structurally_retire_system(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueResponderStore& responder_store,
    const std::string& consumer_id,
    const LocalGooseDialogueCompletionEvent& event,
    const std::int64_t now)
{
    assert(event.speaker_kind == "system");
    const auto disposition = make_local_goose_dialogue_responder_disposition(
        consumer_id,
        event.thread_alias,
        event.sequence,
        event.event_id,
        event.result_sha256,
        event.thread_revision,
        event.task_id,
        event.speaker_kind,
        event.speaker_id,
        LocalGooseDialogueResponderDispositionKind::structural_skip_system,
        "observer-policy-v1",
        now);
    const auto prepared = responder_store.prepare_disposition(disposition);
    assert(prepared.result == LocalGooseDialogueResponderStoreResult::accepted);
    const auto ack = completion_store.acknowledge(consumer_id, event.sequence);
    assert(ack.result == LocalGooseDialogueCompletionFeedResult::accepted);
    const auto reconciled = responder_store.acknowledge_disposition(
        disposition.disposition_id, now);
    assert(reconciled.result == LocalGooseDialogueResponderStoreResult::accepted);
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
    const auto initial_human = succeed_v2(
        make_local_goose_dialogue_v2_successor_task(
            "responder-head", "Continue.", model, root),
        "Je continue.");
    task_store.save(root);
    task_store.save(initial_human);

    const LocalGooseDialogueThreadHead initial_head{
        "main", 0, root.id, initial_human.id};
    assert(thread_store.seed(initial_head).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto first_event = append_completed(
        completion_store, task_store, initial_human.id, 0, 1000);
    assert(first_event.sequence == 1);
    assert(first_event.speaker_kind == "human");

    std::int64_t now = 2000;
    LocalGooseDialogueResponderDispatcher dispatcher(
        runtime, task_store, thread_store, completion_store, responder_store,
        model, "responder-test", [&now] { return now; });

    const auto lease_result = dispatcher.create_lease(
        "lease-responder-two-turn", "main", "sol", "feedback",
        "Two explicit human interventions only", 2, 10000, 100);
    assert(lease_result.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(lease_result.lease);
    const auto lease_id = lease_result.lease->lease_id;

    const auto prepared1 = dispatcher.prepare(
        lease_id, first_event.sequence,
        "feedback", "Première intervention explicite.", 5000);
    assert(prepared1.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared1.intent);
    const auto first_disposition = responder_store.find_disposition_for_event(
        "responder-test", first_event.event_id);
    assert(first_disposition);
    assert(first_disposition->kind
        == LocalGooseDialogueResponderDispositionKind::intervention);
    assert(first_disposition->state
        == LocalGooseDialogueResponderDispositionState::intervention_bound);
    assert(first_disposition->intent_id
        == std::optional<std::string>{prepared1.intent->intent_id});
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{0});

    const auto dispatched1 = dispatcher.dispatch(prepared1.intent->intent_id);
    assert(dispatched1.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(dispatched1.task && dispatched1.head);
    assert(dispatched1.consumer_last_sequence == 1);
    assert(responder_store.find_lease(lease_id)->turns_committed == 1);
    const auto completed_disposition1 =
        responder_store.find_disposition_for_event(
            "responder-test", first_event.event_id);
    assert(completed_disposition1);
    assert(completed_disposition1->state
        == LocalGooseDialogueResponderDispositionState::completed);

    const auto replay1 = dispatcher.dispatch(prepared1.intent->intent_id);
    assert(replay1.result
        == LocalGooseDialogueResponderDispatchCode::duplicate);
    assert(responder_store.find_lease(lease_id)->turns_committed == 1);

    now = 2010;
    complete_v3(
        runtime, task_store, dispatched1.task->id,
        "Réponse système à la première intervention.");
    const auto first_system_event = append_completed(
        completion_store, task_store, dispatched1.task->id, 1, now);
    assert(first_system_event.sequence == 2);
    assert(first_system_event.speaker_kind == "system");

    now = 2020;
    const auto forbidden_self_chain = dispatcher.prepare(
        lease_id, first_system_event.sequence,
        "feedback", "Ceci ne doit jamais être admis.", 5000);
    assert(forbidden_self_chain.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(!forbidden_self_chain.intent);
    assert(!responder_store.find_disposition_for_event(
        "responder-test", first_system_event.event_id));
    assert(responder_store.find_lease(lease_id)->turns_committed == 1);

    structurally_retire_system(
        completion_store, responder_store, "responder-test",
        first_system_event, now);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{2});

    const auto human2 = submit_local_goose_dialogue_v3_preferred(
        runtime, task_store, thread_store, model,
        "human-turn-two", "main", 1,
        "human", "bertrand", "dialogue", "Deuxième tour humain.");
    assert(human2.result
        == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(human2.task && human2.head && human2.head->revision == 2);
    now = 2030;
    complete_v3(runtime, task_store, human2.task->id, "Deuxième message humain.");
    const auto second_human_event = append_completed(
        completion_store, task_store, human2.task->id, 2, now);
    assert(second_human_event.sequence == 3);
    assert(second_human_event.speaker_kind == "human");

    now = 2050;
    const auto too_early = dispatcher.prepare(
        lease_id, second_human_event.sequence,
        "feedback", "Trop tôt pour l'intervalle.", 5000);
    assert(too_early.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(!too_early.intent);
    assert(!responder_store.find_disposition_for_event(
        "responder-test", second_human_event.event_id));
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{2});

    now = 2100;
    const auto prepared2 = dispatcher.prepare(
        lease_id, second_human_event.sequence,
        "feedback", "Seconde intervention explicite.", 5000);
    assert(prepared2.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared2.intent);
    const auto bound2 = responder_store.find_disposition_for_event(
        "responder-test", second_human_event.event_id);
    assert(bound2);
    assert(bound2->state
        == LocalGooseDialogueResponderDispositionState::intervention_bound);
    assert(bound2->intent_id
        == std::optional<std::string>{prepared2.intent->intent_id});

    const auto submitted_before_accounting =
        submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model,
            prepared2.intent->request_id,
            prepared2.intent->thread_alias,
            prepared2.intent->expected_thread_revision,
            prepared2.intent->speaker_kind,
            prepared2.intent->speaker_id,
            prepared2.intent->message_kind,
            prepared2.intent->message);
    assert(submitted_before_accounting.result
        == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(submitted_before_accounting.task
        && submitted_before_accounting.head
        && submitted_before_accounting.head->revision == 3);
    assert(responder_store.find_lease(lease_id)->turns_committed == 1);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{2});

    now = 2101;
    const auto recovered2 = dispatcher.dispatch(prepared2.intent->intent_id);
    assert(recovered2.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(recovered2.task && recovered2.head);
    assert(recovered2.consumer_last_sequence == 3);
    const auto exhausted = responder_store.find_lease(lease_id);
    assert(exhausted);
    assert(exhausted->turns_committed == 2);
    assert(exhausted->state
        == LocalGooseDialogueResponderLeaseState::exhausted);
    assert(exhausted->terminal_reason == "quota_exhausted");
    const auto completed_disposition2 =
        responder_store.find_disposition_for_event(
            "responder-test", second_human_event.event_id);
    assert(completed_disposition2);
    assert(completed_disposition2->state
        == LocalGooseDialogueResponderDispositionState::completed);

    const auto replay2 = dispatcher.dispatch(prepared2.intent->intent_id);
    assert(replay2.result
        == LocalGooseDialogueResponderDispatchCode::duplicate);
    assert(responder_store.find_lease(lease_id)->turns_committed == 2);

    now = 2110;
    complete_v3(
        runtime, task_store, recovered2.task->id,
        "Réponse système à la seconde intervention.");
    const auto second_system_event = append_completed(
        completion_store, task_store, recovered2.task->id, 3, now);
    assert(second_system_event.sequence == 4);
    assert(second_system_event.speaker_kind == "system");
    structurally_retire_system(
        completion_store, responder_store, "responder-test",
        second_system_event, now);
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{4});

    const auto human3 = submit_local_goose_dialogue_v3_preferred(
        runtime, task_store, thread_store, model,
        "human-turn-three", "main", 3,
        "human", "bertrand", "dialogue", "Troisième tour humain.");
    assert(human3.result
        == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(human3.task && human3.head && human3.head->revision == 4);
    now = 2120;
    complete_v3(runtime, task_store, human3.task->id, "Troisième message humain.");
    const auto third_human_event = append_completed(
        completion_store, task_store, human3.task->id, 4, now);
    assert(third_human_event.sequence == 5);
    assert(third_human_event.speaker_kind == "human");

    const auto after_quota = dispatcher.prepare(
        lease_id, third_human_event.sequence,
        "feedback", "Aucune troisième intervention.", 5000);
    assert(after_quota.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(!after_quota.intent);
    assert(!responder_store.find_disposition_for_event(
        "responder-test", third_human_event.event_id));
    assert(completion_store.consumer_last_sequence("responder-test")
        == std::optional<std::uint64_t>{4});
    assert(!responder_store.find_active_lease("main"));

    const auto prepared_dispositions = responder_store.dispositions_in_state(
        LocalGooseDialogueResponderDispositionState::prepared);
    const auto bound_dispositions = responder_store.dispositions_in_state(
        LocalGooseDialogueResponderDispositionState::intervention_bound);
    assert(prepared_dispositions.empty());
    assert(bound_dispositions.empty());

    std::cout
        << "local_goose_dialogue_responder_dispatcher_test: PASS\n";
    return 0;
}
