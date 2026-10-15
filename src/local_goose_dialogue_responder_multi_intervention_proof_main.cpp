#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderObserver.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV3.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;

struct Options {
    std::string state;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string responder_sidecar;
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void require(const bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

Options parse_options(const int argc, char* argv[])
{
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--state") out.state = need("--state");
        else if (arg == "--thread-sidecar") {
            out.thread_sidecar = need("--thread-sidecar");
        } else if (arg == "--completion-sidecar") {
            out.completion_sidecar = need("--completion-sidecar");
        } else if (arg == "--responder-sidecar") {
            out.responder_sidecar = need("--responder-sidecar");
        } else if (arg == "--help") {
            std::cout
                << "Usage: " << argv[0]
                << " --state PATH --thread-sidecar PATH"
                << " --completion-sidecar PATH --responder-sidecar PATH\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    for (const auto* path : {
             &out.state, &out.thread_sidecar,
             &out.completion_sidecar, &out.responder_sidecar}) {
        if (path->empty() || path->front() != '/') {
            throw std::invalid_argument("proof paths must be absolute");
        }
        const auto status = std::filesystem::symlink_status(*path);
        if (!std::filesystem::is_regular_file(status)) {
            throw std::invalid_argument(
                "proof path must be an existing regular non-symlink file");
        }
    }
    return out;
}

void complete_v3(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& task_store,
    const std::string& task_id,
    const std::string& response)
{
    const auto task = task_store.find(task_id);
    require(task.has_value(), "Task to complete is missing");
    const auto request = inspect_local_goose_dialogue_v3_task(*task);
    require(request.eligible, "Task to complete is not canonical V3");
    require(runtime.start(task_id, "multi-intervention-proof"),
            "Task could not be started");
    const auto output = make_local_goose_dialogue_v3_response(
        request, response);
    require(runtime.succeed(
                task_id, output,
                local_goose_dialogue_v3_response_content_type)
            == gaudere::work::FinishResult::accepted,
            "Task could not be completed");
    const auto completed = task_store.find(task_id);
    require(completed && canonical_local_goose_dialogue_v3_success(*completed),
            "completed Task is not canonical V3 success");
}

LocalGooseDialogueCompletionEvent next_event(
    LocalGooseDialogueCompletionFeedStore& feed,
    const std::string& consumer_id,
    const std::uint64_t expected_sequence,
    const std::string& expected_speaker)
{
    const auto event = feed.next_for_consumer(consumer_id);
    require(event.has_value(), "expected next completion event is missing");
    require(event->sequence == expected_sequence,
            "next completion sequence differs");
    require(event->speaker_kind == expected_speaker,
            "next completion speaker kind differs");
    return *event;
}

void reconcile_feed(
    LocalGooseDialogueCompletionFeedService& service,
    const std::size_t expected_materialized)
{
    const auto result = service.reconcile("main", 64);
    require(!result.blocked, "completion feed reconciliation blocked");
    require(result.materialized == expected_materialized,
            "completion feed materialization count differs");
}

void require_intervention_state(
    LocalGooseDialogueResponderStore& store,
    const std::string& consumer_id,
    const std::string& event_id,
    const std::string& intent_id,
    const LocalGooseDialogueResponderDispositionState expected_state)
{
    const auto disposition = store.find_disposition_for_event(
        consumer_id, event_id);
    require(disposition.has_value(), "intervention disposition is missing");
    require(disposition->kind
                == LocalGooseDialogueResponderDispositionKind::intervention,
            "disposition is not intervention");
    require(disposition->state == expected_state,
            "intervention disposition state differs");
    require(disposition->intent_id
                == std::optional<std::string>{intent_id},
            "intervention disposition intent binding differs");
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        constexpr const char* consumer_id = "system-responder-v1";
        constexpr const char* thread_alias = "main";
        constexpr std::uint64_t baseline_cursor = 3;
        constexpr std::uint64_t first_human_sequence = 4;

        gaudere::persistence::sqlite::TaskStore task_store(options.state);
        std::int64_t now = 2000000000000LL;
        gaudere::work::Runtime runtime(
            task_store,
            [&now] {
                return std::chrono::system_clock::time_point{
                    std::chrono::milliseconds{now}};
            });
        runtime.recover();

        LocalGooseDialogueThreadStore thread_store(options.thread_sidecar);
        LocalGooseDialogueCompletionFeedStore completion_store(
            options.completion_sidecar);
        LocalGooseDialogueResponderStore responder_store(
            options.responder_sidecar);

        const auto head0 = thread_store.find(thread_alias);
        require(head0 && head0->revision == 3,
                "proof baseline preferred head is not revision 3");
        const auto head_task0 = task_store.find(head0->head_task_id);
        require(head_task0.has_value(), "proof baseline head Task is missing");
        const auto inspected0 = inspect_local_goose_dialogue_v3_task(*head_task0);
        require(inspected0.eligible && inspected0.speaker_kind == "human",
                "proof baseline head is not current human V3");
        const auto model_sha256 = inspected0.model_sha256;
        require(model_sha256.size() == 64,
                "proof baseline model SHA-256 differs");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{baseline_cursor},
                "proof baseline responder cursor differs");
        const auto event4 = next_event(
            completion_store, consumer_id,
            first_human_sequence, "human");
        require(event4.task_id == head0->head_task_id,
                "proof baseline sequence 4 is not preferred head");
        require(!responder_store.find_active_lease(thread_alias),
                "proof baseline unexpectedly has active responder lease");

        LocalGooseDialogueCompletionFeedService feed_service(
            thread_store,
            completion_store,
            [&task_store](const std::string& id) {
                return task_store.find(id);
            },
            [&now] { return now; });

        LocalGooseDialogueResponderDispatcher dispatcher(
            runtime, task_store, thread_store, completion_store,
            responder_store, model_sha256, consumer_id, [&now] { return now; });

        LocalGooseDialogueResponderObserverConfig observer_config;
        observer_config.consumer_id = consumer_id;
        observer_config.thread_alias = thread_alias;
        observer_config.structural_authority_source = "observer-policy-v1";
        observer_config.observation_armed = true;
        LocalGooseDialogueResponderObserver observer(
            completion_store, thread_store, responder_store,
            observer_config, [&now] { return now; });

        const auto lease = dispatcher.create_lease(
            "multi-intervention-proof-lease-v1",
            thread_alias,
            "system-responder-proof",
            "feedback",
            "Isolated finite two-human-intervention proof",
            2,
            60000,
            100);
        require(lease.result == LocalGooseDialogueResponderDispatchCode::accepted
                    && lease.lease,
                "finite two-turn lease creation failed");
        const auto lease_id = lease.lease->lease_id;

        const auto prepared1 = dispatcher.prepare(
            lease_id, event4.sequence,
            "feedback", "Première intervention isolée.", 10000);
        require(prepared1.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && prepared1.intent,
                "first human intervention preparation failed");
        require_intervention_state(
            responder_store, consumer_id, event4.event_id,
            prepared1.intent->intent_id,
            LocalGooseDialogueResponderDispositionState::intervention_bound);
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{3},
                "prepare consumed first human completion");

        const auto dispatched1 = dispatcher.dispatch(
            prepared1.intent->intent_id);
        require(dispatched1.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && dispatched1.task && dispatched1.head,
                "first human intervention dispatch failed");
        require(dispatched1.consumer_last_sequence == 4,
                "first human trigger was not ACKed exactly once");
        require_intervention_state(
            responder_store, consumer_id, event4.event_id,
            prepared1.intent->intent_id,
            LocalGooseDialogueResponderDispositionState::completed);
        require(responder_store.find_lease(lease_id)->turns_committed == 1,
                "first intervention did not account exactly one lease turn");

        now += 10;
        complete_v3(
            runtime, task_store, dispatched1.task->id,
            "Première réponse système isolée.");
        reconcile_feed(feed_service, 1);
        const auto event5 = next_event(
            completion_store, consumer_id, 5, "system");
        const auto self_chain = dispatcher.prepare(
            lease_id, event5.sequence,
            "feedback", "Auto-chaînage interdit.", 10000);
        require(self_chain.result
                    == LocalGooseDialogueResponderDispatchCode::conflict,
                "system completion incorrectly admitted responder intent");
        require(!self_chain.intent,
                "system completion created responder intent");

        now += 10;
        const auto skip5 = observer.step();
        require(skip5.action
                    == LocalGooseDialogueResponderObserverAction::acknowledged,
                "observer did not structurally retire first system completion");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{5},
                "observer cursor did not advance to first system completion");
        const auto disposition5 = responder_store.find_disposition_for_event(
            consumer_id, event5.event_id);
        require(disposition5
                    && disposition5->kind
                        == LocalGooseDialogueResponderDispositionKind::structural_skip_system
                    && disposition5->state
                        == LocalGooseDialogueResponderDispositionState::acknowledged,
                "first system skip disposition differs");

        const auto human2 = submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model_sha256,
            "multi-proof-human-two", thread_alias,
            dispatched1.head->revision,
            "human", "bertrand-proof", "dialogue",
            "Deuxième événement humain isolé.");
        require(human2.result
                    == LocalGooseDialoguePreferredSubmitResultCode::accepted
                    && human2.task && human2.head,
                "second human preferred submission failed");
        now += 10;
        complete_v3(
            runtime, task_store, human2.task->id,
            "Deuxième événement humain terminé.");
        reconcile_feed(feed_service, 1);
        const auto event6 = next_event(
            completion_store, consumer_id, 6, "human");
        const auto held6 = observer.step();
        require(held6.action == LocalGooseDialogueResponderObserverAction::held,
                "observer did not hold second human completion");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{5},
                "observer ACKed second human completion");

        now = 2000000000050LL;
        const auto too_early = dispatcher.prepare(
            lease_id, event6.sequence,
            "feedback", "Intervention trop précoce.", 10000);
        require(too_early.result
                    == LocalGooseDialogueResponderDispatchCode::conflict,
                "minimum intervention interval did not fail closed");
        require(!responder_store.find_disposition_for_event(
                    consumer_id, event6.event_id),
                "too-early preparation created a disposition");

        now = 2000000000100LL;
        const auto prepared2 = dispatcher.prepare(
            lease_id, event6.sequence,
            "feedback", "Seconde intervention isolée.", 10000);
        require(prepared2.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && prepared2.intent,
                "second human intervention preparation failed");
        require(prepared2.intent->intent_id != prepared1.intent->intent_id,
                "second human event reused first intent identity");
        require_intervention_state(
            responder_store, consumer_id, event6.event_id,
            prepared2.intent->intent_id,
            LocalGooseDialogueResponderDispositionState::intervention_bound);

        const auto submitted_before_accounting =
            submit_local_goose_dialogue_v3_preferred(
                runtime, task_store, thread_store, model_sha256,
                prepared2.intent->request_id,
                prepared2.intent->thread_alias,
                prepared2.intent->expected_thread_revision,
                prepared2.intent->speaker_kind,
                prepared2.intent->speaker_id,
                prepared2.intent->message_kind,
                prepared2.intent->message);
        require(submitted_before_accounting.result
                    == LocalGooseDialoguePreferredSubmitResultCode::accepted
                    && submitted_before_accounting.task
                    && submitted_before_accounting.head,
                "crash-window preferred submission failed");
        require(responder_store.find_lease(lease_id)->turns_committed == 1,
                "crash-window submission accounted a lease turn early");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{5},
                "crash-window submission ACKed human trigger early");

        now += 1;
        const auto recovered2 = dispatcher.dispatch(
            prepared2.intent->intent_id);
        require(recovered2.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && recovered2.task && recovered2.head,
                "second intervention crash recovery failed");
        require(recovered2.consumer_last_sequence == 6,
                "second human trigger was not ACKed exactly once");
        require_intervention_state(
            responder_store, consumer_id, event6.event_id,
            prepared2.intent->intent_id,
            LocalGooseDialogueResponderDispositionState::completed);
        const auto exhausted = responder_store.find_lease(lease_id);
        require(exhausted
                    && exhausted->turns_committed == 2
                    && exhausted->state
                        == LocalGooseDialogueResponderLeaseState::exhausted,
                "two-turn lease did not become exhausted");

        const auto replay2 = dispatcher.dispatch(prepared2.intent->intent_id);
        require(replay2.result
                    == LocalGooseDialogueResponderDispatchCode::duplicate,
                "second intervention retry was not idempotent");
        require(responder_store.find_lease(lease_id)->turns_committed == 2,
                "second intervention retry double-accounted lease turn");

        now += 10;
        complete_v3(
            runtime, task_store, recovered2.task->id,
            "Seconde réponse système isolée.");
        reconcile_feed(feed_service, 1);
        const auto event7 = next_event(
            completion_store, consumer_id, 7, "system");
        const auto skip7 = observer.step();
        require(skip7.action
                    == LocalGooseDialogueResponderObserverAction::acknowledged,
                "observer did not retire second system completion");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{7},
                "observer cursor did not advance to second system completion");

        const auto human3 = submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model_sha256,
            "multi-proof-human-three", thread_alias,
            recovered2.head->revision,
            "human", "bertrand-proof", "dialogue",
            "Troisième événement humain isolé.");
        require(human3.result
                    == LocalGooseDialoguePreferredSubmitResultCode::accepted
                    && human3.task && human3.head,
                "third human preferred submission failed");
        now += 10;
        complete_v3(
            runtime, task_store, human3.task->id,
            "Troisième événement humain terminé.");
        reconcile_feed(feed_service, 1);
        const auto event8 = next_event(
            completion_store, consumer_id, 8, "human");
        const auto held8 = observer.step();
        require(held8.action == LocalGooseDialogueResponderObserverAction::held,
                "observer did not hold third human completion");
        require(completion_store.consumer_last_sequence(consumer_id)
                    == std::optional<std::uint64_t>{7},
                "observer ACKed third human completion");

        const auto third_intervention = dispatcher.prepare(
            lease_id, event8.sequence,
            "feedback", "Troisième intervention interdite.", 10000);
        require(third_intervention.result
                    == LocalGooseDialogueResponderDispatchCode::conflict,
                "exhausted lease admitted a third intervention");
        require(!third_intervention.intent,
                "exhausted lease created a third intent");
        require(!responder_store.find_disposition_for_event(
                    consumer_id, event8.event_id),
                "exhausted lease created a third intervention disposition");
        require(!responder_store.find_active_lease(thread_alias),
                "proof auto-renewed or replaced exhausted lease");

        const auto final_status = observer.inspect();
        require(final_status.classification
                    == LocalGooseDialogueResponderObserverClassification::current_human
                    && final_status.next_event
                    && final_status.next_event->sequence == 8,
                "final observer status is not held current human sequence 8");

        std::cout
            << "BASELINE_RESPONDER_CURSOR=3\n"
            << "FIRST_HUMAN_SEQUENCE=4\n"
            << "FIRST_SYSTEM_SEQUENCE=5\n"
            << "SECOND_HUMAN_SEQUENCE=6\n"
            << "SECOND_SYSTEM_SEQUENCE=7\n"
            << "FINAL_HUMAN_SEQUENCE=8\n"
            << "LEASE_TURNS_COMMITTED=2\n"
            << "LEASE_STATE=exhausted\n"
            << "FINAL_RESPONDER_CURSOR=7\n"
            << "FINAL_CLASSIFICATION=current_human\n"
            << "SELF_CHAIN=blocked\n"
            << "AUTO_RENEW=blocked\n"
            << "MIN_INTERVAL=fail-closed\n"
            << "CRASH_RETRY=idempotent\n"
            << "GAUDERE_DIALOGUE_RESPONDER_MULTI_INTERVENTION_PROOF=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere dialogue responder multi-intervention proof: FAIL: "
            << error.what() << '\n';
        return 1;
    }
}
