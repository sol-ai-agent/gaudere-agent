#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderAuthorityService.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
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
    require(runtime.start(task_id, "decision-authority-proof"),
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

LocalGooseDialogueResponderDecisionPrepareRequest exact_request(
    const std::string& request_id,
    const std::string& message,
    const LocalGooseDialogueResponderGrant& grant,
    const LocalGooseDialogueResponderLease& lease,
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueThreadHead& head)
{
    LocalGooseDialogueResponderDecisionPrepareRequest out;
    out.request_id = request_id;
    out.grant_id = grant.grant_id;
    out.lease_id = lease.lease_id;
    out.consumer_id = "system-responder-v1";
    out.completion_sequence = event.sequence;
    out.event_id = event.event_id;
    out.result_sha256 = event.result_sha256;
    out.thread_alias = event.thread_alias;
    out.thread_revision = event.thread_revision;
    out.task_id = event.task_id;
    out.observed_speaker_kind = event.speaker_kind;
    out.observed_speaker_id = event.speaker_id;
    out.expected_head_revision = head.revision;
    out.expected_head_task_id = head.head_task_id;
    out.message_kind = "feedback";
    out.message = message;
    out.intent_ttl_ms = 10000;
    return out;
}

void require_cursor(
    LocalGooseDialogueCompletionFeedStore& store,
    const std::uint64_t sequence)
{
    require(store.consumer_last_sequence("system-responder-v1")
                == std::optional<std::uint64_t>{sequence},
            "responder consumer cursor differs");
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        constexpr const char* consumer_id = "system-responder-v1";
        constexpr const char* thread_alias = "main";

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
        require_cursor(completion_store, 3);
        const auto event4 = next_event(
            completion_store, consumer_id, 4, "human");
        require(event4.task_id == head0->head_task_id,
                "proof baseline sequence 4 is not preferred head");

        {
            LocalGooseDialogueResponderStore base_before(
                options.responder_sidecar);
            require(!base_before.find_active_lease(thread_alias),
                    "proof baseline unexpectedly has active responder lease");
            LocalGooseDialogueResponderAuthorityStore authority_before(
                options.responder_sidecar);
            require(authority_before.schema_version() == 2,
                    "copied responder sidecar is not schema v2 before proof migration");
        }
        {
            LocalGooseDialogueResponderAuthorityStore migrator(
                options.responder_sidecar,
                LocalGooseDialogueResponderAuthorityStoreOpenMode::
                    allow_schema_upgrade);
            require(migrator.schema_version() == 3,
                    "explicit isolated authority schema migration failed");
            require(migrator.grants_in_state(
                        LocalGooseDialogueResponderGrantState::prepared).empty(),
                    "isolated migration synthesized historic grants");
            require(migrator.decisions_in_state(
                        LocalGooseDialogueResponderDecisionState::prepared).empty(),
                    "isolated migration synthesized historic decisions");
        }

        LocalGooseDialogueResponderStore responder_store(
            options.responder_sidecar);
        LocalGooseDialogueResponderAuthorityStore authority_store(
            options.responder_sidecar);
        require(authority_store.schema_version() == 3,
                "base/authority schema v3 reopen failed on copied state");

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

        LocalGooseDialogueResponderAuthorityServiceConfig config;
        config.authority_enabled = true;
        LocalGooseDialogueResponderAuthorityService authority(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, [&now] { return now; });

        const auto initial_status = authority.inspect(thread_alias);
        require(initial_status.next_event
                    && initial_status.next_event->sequence == 4,
                "authority status does not hold production-copy sequence 4");
        require(!initial_status.prepared_grant
                    && !initial_status.activated_grant
                    && !initial_status.active_lease
                    && !initial_status.event_decision,
                "authority code presence synthesized authority state");
        require_cursor(completion_store, 3);

        LocalGooseDialogueResponderGrantPrepareRequest grant_request;
        grant_request.request_id = "decision-authority-proof-grant-v1";
        grant_request.thread_alias = thread_alias;
        grant_request.speaker_id = "sol-proof";
        grant_request.allowed_message_kinds =
            local_goose_dialogue_responder_message_feedback;
        grant_request.purpose =
            "Isolated exact-decision authority proof on copied production state";
        grant_request.max_system_turns = 2;
        grant_request.max_lease_ttl_ms = 30000;
        grant_request.min_interval_ms = 100;
        grant_request.grant_ttl_ms = 60000;

        const auto prepared_grant = authority.prepare_grant(grant_request);
        require(prepared_grant.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && prepared_grant.grant,
                "explicit grant preparation failed");
        const auto grant_id = prepared_grant.grant->grant_id;
        require(prepared_grant.grant->state
                    == LocalGooseDialogueResponderGrantState::prepared,
                "prepared grant state differs");
        require(!responder_store.find_active_lease(thread_alias),
                "prepared grant auto-activated a lease");
        require_cursor(completion_store, 3);

        {
            LocalGooseDialogueResponderAuthorityStore reopened_authority(
                options.responder_sidecar);
            LocalGooseDialogueResponderStore reopened_base(
                options.responder_sidecar);
            const auto durable_grant = reopened_authority.find_grant(grant_id);
            require(durable_grant
                        && durable_grant->state
                            == LocalGooseDialogueResponderGrantState::prepared,
                    "prepared grant did not survive reopen");
            require(!reopened_base.find_active_lease(thread_alias),
                    "reopen auto-activated a prepared grant");
        }
        LocalGooseDialogueResponderAuthorityService restarted_before_activation(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, [&now] { return now; });
        require(!restarted_before_activation.inspect(thread_alias).active_lease,
                "service reconstruction auto-activated a prepared grant");

        now += 1;
        const auto activated = restarted_before_activation.activate_grant(grant_id);
        require(activated.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && activated.grant && activated.lease,
                "explicit grant activation failed");
        const auto grant = *activated.grant;
        const auto lease = *activated.lease;
        require(grant.state == LocalGooseDialogueResponderGrantState::activated,
                "activated grant state differs");
        require(lease.turns_committed == 0 && lease.max_system_turns == 2,
                "activated finite lease differs");
        require_cursor(completion_store, 3);

        auto request1 = exact_request(
            "decision-authority-proof-decision-1",
            "Première intervention explicitement décidée.",
            grant, lease, event4, *head0);
        now += 1;
        const auto decision1_write = authority.prepare_decision(request1);
        require(decision1_write.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && decision1_write.decision,
                "first exact human decision preparation failed");
        const auto decision1_id = decision1_write.decision->decision_id;
        require(decision1_write.decision->state
                    == LocalGooseDialogueResponderDecisionState::prepared,
                "first decision state differs after prepare");
        require(!responder_store.find_disposition_for_event(
                    consumer_id, event4.event_id),
                "decision preparation created an intervention disposition");
        require_cursor(completion_store, 3);
        require(thread_store.find(thread_alias)->revision == 3,
                "decision preparation advanced preferred head");

        {
            LocalGooseDialogueResponderAuthorityStore reopened_authority(
                options.responder_sidecar);
            LocalGooseDialogueResponderStore reopened_base(
                options.responder_sidecar);
            const auto durable_decision =
                reopened_authority.find_decision(decision1_id);
            require(durable_decision
                        && durable_decision->state
                            == LocalGooseDialogueResponderDecisionState::prepared,
                    "prepared decision did not survive reopen");
            require(!reopened_base.find_disposition_for_event(
                        consumer_id, event4.event_id),
                    "reopen auto-bound a prepared decision");
        }
        LocalGooseDialogueResponderAuthorityService restarted_before_bind(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, [&now] { return now; });
        require(restarted_before_bind.inspect(thread_alias).event_decision
                    && restarted_before_bind.inspect(thread_alias)
                           .event_decision->state
                        == LocalGooseDialogueResponderDecisionState::prepared,
                "service reconstruction mutated prepared decision");
        require_cursor(completion_store, 3);

        now += 1;
        const auto bound1 = restarted_before_bind.bind_decision(decision1_id);
        require(bound1.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && bound1.decision && bound1.intent,
                "first explicit decision bind failed");
        require(bound1.decision->state
                    == LocalGooseDialogueResponderDecisionState::intent_bound
                    && bound1.intent->state
                        == LocalGooseDialogueResponderIntentState::prepared,
                "first bound decision/intent state differs");
        require(bound1.intent->submitted_task_id.empty(),
                "decision bind submitted a Task");
        require_cursor(completion_store, 3);
        require(thread_store.find(thread_alias)->revision == 3,
                "decision bind advanced preferred head");
        require(responder_store.find_lease(lease.lease_id)->turns_committed == 0,
                "decision bind consumed a lease turn");

        {
            LocalGooseDialogueResponderAuthorityStore reopened_authority(
                options.responder_sidecar);
            LocalGooseDialogueResponderStore reopened_base(
                options.responder_sidecar);
            const auto durable_decision =
                reopened_authority.find_decision(decision1_id);
            require(durable_decision
                        && durable_decision->state
                            == LocalGooseDialogueResponderDecisionState::intent_bound,
                    "intent-bound decision did not survive reopen");
            require(durable_decision->intent_id
                        && reopened_base.find_intent(*durable_decision->intent_id),
                    "bound intent did not survive reopen");
        }
        LocalGooseDialogueResponderAuthorityService restarted_after_bind(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, [&now] { return now; });
        require_cursor(completion_store, 3);
        require(thread_store.find(thread_alias)->revision == 3,
                "service reconstruction dispatched an intent-bound decision");
        require(responder_store.find_lease(lease.lease_id)->turns_committed == 0,
                "service reconstruction consumed a lease turn");

        now += 1;
        const auto dispatched1 = dispatcher.dispatch(bound1.intent->intent_id);
        require(dispatched1.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && dispatched1.task && dispatched1.head,
                "first explicitly bound intervention dispatch failed");
        require(dispatched1.consumer_last_sequence == 4,
                "first isolated human trigger was not ACKed exactly once");
        require(responder_store.find_lease(lease.lease_id)->turns_committed == 1,
                "first intervention did not consume exactly one lease turn");

        now += 10;
        complete_v3(
            runtime, task_store, dispatched1.task->id,
            "Première réponse système isolée.");
        reconcile_feed(feed_service, 1);
        const auto event5 = next_event(
            completion_store, consumer_id, 5, "system");
        const auto head5 = thread_store.find(thread_alias);
        require(head5 && head5->head_task_id == event5.task_id,
                "system completion is not current preferred head");

        const auto system_decision = authority.prepare_decision(exact_request(
            "decision-authority-proof-system-decision",
            "Une complétion système ne doit jamais se décider elle-même.",
            grant, *responder_store.find_lease(lease.lease_id),
            event5, *head5));
        require(system_decision.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::conflict
                    && !system_decision.decision,
                "system completion admitted a durable intervention decision");
        require(!authority_store.find_decision_for_event(
                    consumer_id, event5.event_id),
                "system completion created a decision row");

        const auto skip5 = observer.step();
        require(skip5.action
                    == LocalGooseDialogueResponderObserverAction::acknowledged,
                "observer did not structurally retire first system completion");
        require_cursor(completion_store, 5);

        const auto human2 = submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model_sha256,
            "decision-authority-proof-human-two", thread_alias,
            head5->revision,
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
        require_cursor(completion_store, 5);
        require(!authority_store.find_decision_for_event(
                    consumer_id, event6.event_id),
                "previous intervention synthesized the next human decision");

        now += 200;
        const auto lease_after_first = responder_store.find_lease(lease.lease_id);
        const auto head6 = thread_store.find(thread_alias);
        require(lease_after_first && head6
                    && head6->head_task_id == event6.task_id,
                "second human exact evidence is unavailable");
        const auto decision2_write = authority.prepare_decision(exact_request(
            "decision-authority-proof-decision-2",
            "Seconde intervention avec décision fraîche distincte.",
            grant, *lease_after_first, event6, *head6));
        require(decision2_write.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && decision2_write.decision,
                "second fresh human decision preparation failed");
        require(decision2_write.decision->decision_id != decision1_id,
                "second human event reused first decision identity");
        const auto bound2 = authority.bind_decision(
            decision2_write.decision->decision_id);
        require(bound2.result
                    == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                    && bound2.intent,
                "second fresh decision bind failed");
        require_cursor(completion_store, 5);

        now += 1;
        const auto dispatched2 = dispatcher.dispatch(bound2.intent->intent_id);
        require(dispatched2.result
                    == LocalGooseDialogueResponderDispatchCode::accepted
                    && dispatched2.task && dispatched2.head,
                "second explicitly decided intervention dispatch failed");
        require(dispatched2.consumer_last_sequence == 6,
                "second isolated human trigger was not ACKed exactly once");
        const auto exhausted = responder_store.find_lease(lease.lease_id);
        require(exhausted
                    && exhausted->turns_committed == 2
                    && exhausted->state
                        == LocalGooseDialogueResponderLeaseState::exhausted,
                "finite two-turn authority lease did not exhaust");

        now += 10;
        complete_v3(
            runtime, task_store, dispatched2.task->id,
            "Seconde réponse système isolée.");
        reconcile_feed(feed_service, 1);
        const auto event7 = next_event(
            completion_store, consumer_id, 7, "system");
        const auto skip7 = observer.step();
        require(skip7.action
                    == LocalGooseDialogueResponderObserverAction::acknowledged,
                "observer did not structurally retire second system completion");
        require_cursor(completion_store, 7);

        const auto human3 = submit_local_goose_dialogue_v3_preferred(
            runtime, task_store, thread_store, model_sha256,
            "decision-authority-proof-human-three", thread_alias,
            dispatched2.head->revision,
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
        require_cursor(completion_store, 7);
        require(!responder_store.find_active_lease(thread_alias),
                "exhausted authority lease was auto-renewed or replaced");
        require(!authority_store.find_decision_for_event(
                    consumer_id, event8.event_id),
                "third human event received an automatic decision");

        const auto final_status = authority.inspect(thread_alias);
        require(final_status.next_event
                    && final_status.next_event->sequence == 8
                    && final_status.next_event->speaker_kind == "human",
                "final authority status does not hold current human sequence 8");
        require(!final_status.active_lease,
                "final authority status unexpectedly has active lease");

        std::cout
            << "BASELINE_RESPONDER_SCHEMA=2\n"
            << "ISOLATED_AUTHORITY_SCHEMA=3\n"
            << "BASELINE_RESPONDER_CURSOR=3\n"
            << "FIRST_HUMAN_SEQUENCE=4\n"
            << "PREPARED_GRANT_RESTART=no-activation\n"
            << "PREPARED_DECISION_RESTART=no-bind\n"
            << "INTENT_BOUND_RESTART=no-dispatch\n"
            << "SYSTEM_DECISION=blocked\n"
            << "FRESH_DECISION_PER_HUMAN=proved\n"
            << "LEASE_TURNS_COMMITTED=2\n"
            << "LEASE_STATE=exhausted\n"
            << "AUTO_RENEW=blocked\n"
            << "FINAL_RESPONDER_CURSOR=7\n"
            << "FINAL_PENDING_SEQUENCE=8\n"
            << "FINAL_CLASSIFICATION=current_human\n"
            << "GAUDERE_DIALOGUE_RESPONDER_DECISION_AUTHORITY_PROOF=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere dialogue responder decision authority proof: FAIL: "
            << error.what() << '\n';
        return 1;
    }
}
