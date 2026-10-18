#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderAuthorityService.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
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
                   std::chrono::steady_clock::now().time_since_epoch().count())
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
    assert(runtime.start(task_id, "authority-service-test"));
    const auto output = make_local_goose_dialogue_v3_response(
        inspected, response);
    assert(runtime.succeed(
        task_id, output, local_goose_dialogue_v3_response_content_type)
        == gaudere::work::FinishResult::accepted);
}

LocalGooseDialogueResponderDecisionPrepareRequest exact_request(
    const LocalGooseDialogueResponderGrant& grant,
    const LocalGooseDialogueResponderLease& lease,
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueThreadHead& head)
{
    LocalGooseDialogueResponderDecisionPrepareRequest out;
    out.request_id = "authority-decision-one";
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
    out.message = "Intervention explicitement décidée par la surface typée.";
    out.intent_ttl_ms = 5000;
    return out;
}

} // namespace

int main()
{
    TemporaryFile core{"gaudere-authority-service-core"};
    TemporaryFile thread{"gaudere-authority-service-thread"};
    TemporaryFile feed{"gaudere-authority-service-feed"};
    TemporaryFile responder{"gaudere-authority-service-responder"};

    gaudere::persistence::sqlite::TaskStore task_store(core.path.string());
    gaudere::work::Runtime runtime(
        task_store, [] { return std::chrono::system_clock::now(); });
    runtime.recover();

    LocalGooseDialogueThreadStore thread_store(thread.path.string());
    LocalGooseDialogueCompletionFeedStore completion_store(feed.path.string());
    LocalGooseDialogueResponderStore responder_store(responder.path.string());
    LocalGooseDialogueResponderAuthorityStore authority_store(
        responder.path.string(),
        LocalGooseDialogueResponderAuthorityStoreOpenMode::allow_schema_upgrade);
    assert(authority_store.schema_version() == 3);

    const auto model = model_sha();
    const auto root = succeed_v2(
        make_local_goose_dialogue_v2_root_task(
            "authority-service-root", "Bonjour.", model),
        "Bonjour.");
    const auto v2_head = succeed_v2(
        make_local_goose_dialogue_v2_successor_task(
            "authority-service-v2-head", "Continue.", model, root),
        "Je continue.");
    task_store.save(root);
    task_store.save(v2_head);
    assert(thread_store.seed(LocalGooseDialogueThreadHead{
        "main", 0, root.id, v2_head.id}).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto human = submit_local_goose_dialogue_v3_preferred(
        runtime, task_store, thread_store, model,
        "authority-service-human-one", "main", 0,
        "human", "bertrand", "dialogue", "Événement humain courant.");
    assert(human.result == LocalGooseDialoguePreferredSubmitResultCode::accepted);
    assert(human.task && human.head && human.head->revision == 1);
    complete_v3(runtime, task_store, human.task->id, "Événement humain terminé.");
    const auto completed_human = task_store.find(human.task->id);
    assert(completed_human);
    const auto event_write = completion_store.append_for_revision(
        make_local_goose_dialogue_completion_event(
            "main", 1, *completed_human, 4000));
    assert(event_write.result == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(event_write.event && event_write.event->sequence == 1);
    const auto event = *event_write.event;
    const auto head = thread_store.find("main");
    assert(head && head->revision == 1 && head->head_task_id == event.task_id);

    std::int64_t now = 5000;
    LocalGooseDialogueResponderDispatcher dispatcher(
        runtime, task_store, thread_store, completion_store,
        responder_store, model, "system-responder-v1", [&now] { return now; });

    LocalGooseDialogueResponderAuthorityServiceConfig disabled_config;
    disabled_config.authority_enabled = false;
    LocalGooseDialogueResponderAuthorityService disabled(
        completion_store, thread_store, responder_store, authority_store,
        dispatcher, disabled_config, [&now] { return now; });

    LocalGooseDialogueResponderGrantPrepareRequest grant_request;
    grant_request.request_id = "authority-grant-one";
    grant_request.thread_alias = "main";
    grant_request.speaker_id = "sol";
    grant_request.allowed_message_kinds =
        local_goose_dialogue_responder_message_feedback;
    grant_request.purpose = "Typed authority service proof";
    grant_request.max_system_turns = 2;
    grant_request.max_lease_ttl_ms = 10000;
    grant_request.min_interval_ms = 100;
    grant_request.grant_ttl_ms = 20000;

    assert(disabled.prepare_grant(grant_request).result
        == LocalGooseDialogueResponderAuthorityServiceCode::disabled);
    assert(authority_store.grants_in_state(
        LocalGooseDialogueResponderGrantState::prepared).empty());
    assert(!responder_store.find_active_lease("main"));
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));

    LocalGooseDialogueResponderAuthorityServiceConfig enabled_config;
    enabled_config.authority_enabled = true;
    LocalGooseDialogueResponderAuthorityService service(
        completion_store, thread_store, responder_store, authority_store,
        dispatcher, enabled_config, [&now] { return now; });

    const auto prepared_grant = service.prepare_grant(grant_request);
    assert(prepared_grant.result
        == LocalGooseDialogueResponderAuthorityServiceCode::accepted);
    assert(prepared_grant.grant);
    const auto grant = *prepared_grant.grant;
    assert(grant.state == LocalGooseDialogueResponderGrantState::prepared);
    assert(!responder_store.find_active_lease("main"));

    const auto status_prepared = service.inspect("main");
    assert(status_prepared.authority_enabled);
    assert(status_prepared.next_event
        && status_prepared.next_event->event_id == event.event_id);
    assert(status_prepared.prepared_grant
        && status_prepared.prepared_grant->grant_id == grant.grant_id);
    assert(!status_prepared.active_lease);
    assert(!status_prepared.event_decision);
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));

    // Reconstructing the service does not activate a prepared grant.
    LocalGooseDialogueResponderAuthorityService restarted_before_activation(
        completion_store, thread_store, responder_store, authority_store,
        dispatcher, enabled_config, [&now] { return now; });
    const auto restart_status = restarted_before_activation.inspect("main");
    assert(restart_status.prepared_grant
        && restart_status.prepared_grant->grant_id == grant.grant_id);
    assert(!restart_status.active_lease);

    now = 5100;
    const auto activated = restarted_before_activation.activate_grant(grant.grant_id);
    assert(activated.result
        == LocalGooseDialogueResponderAuthorityServiceCode::accepted);
    assert(activated.grant && activated.lease);
    const auto lease = *activated.lease;
    assert(activated.grant->state
        == LocalGooseDialogueResponderGrantState::activated);
    assert(lease.allowed_message_kinds == grant.allowed_message_kinds);
    assert(lease.max_system_turns == grant.max_system_turns);
    assert(lease.min_interval_ms == grant.min_interval_ms);
    assert(lease.purpose == grant.purpose);
    assert(lease.turns_committed == 0);

    now = 5150;
    const auto activation_retry = service.activate_grant(grant.grant_id);
    assert(activation_retry.result
        == LocalGooseDialogueResponderAuthorityServiceCode::duplicate);
    assert(activation_retry.lease
        && activation_retry.lease->lease_id == lease.lease_id);
    assert(responder_store.find_active_lease("main")->lease_id == lease.lease_id);

    auto decision_request = exact_request(
        *activation_retry.grant, lease, event, *head);
    auto tampered = decision_request;
    tampered.result_sha256 = std::string(64, 'f');
    assert(service.prepare_decision(tampered).result
        == LocalGooseDialogueResponderAuthorityServiceCode::conflict);
    assert(authority_store.decisions_in_state(
        LocalGooseDialogueResponderDecisionState::prepared).empty());

    now = 5200;
    const auto prepared_decision = service.prepare_decision(decision_request);
    assert(prepared_decision.result
        == LocalGooseDialogueResponderAuthorityServiceCode::accepted);
    assert(prepared_decision.decision);
    const auto decision = *prepared_decision.decision;
    assert(decision.state == LocalGooseDialogueResponderDecisionState::prepared);
    assert(!responder_store.find_intent(
        std::string{local_goose_dialogue_responder_intent_prefix}
        + std::string(64, '0')));
    assert(!responder_store.find_disposition_for_event(
        "system-responder-v1", event.event_id));
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));
    assert(thread_store.find("main")->revision == 1);

    now = 5250;
    const auto decision_retry = service.prepare_decision(decision_request);
    assert(decision_retry.result
        == LocalGooseDialogueResponderAuthorityServiceCode::duplicate);
    assert(decision_retry.decision
        && decision_retry.decision->decision_id == decision.decision_id);

    now = 5300;
    const auto bound = service.bind_decision(decision.decision_id);
    assert(bound.result == LocalGooseDialogueResponderAuthorityServiceCode::accepted);
    assert(bound.decision && bound.intent);
    assert(bound.decision->state
        == LocalGooseDialogueResponderDecisionState::intent_bound);
    assert(bound.intent->state == LocalGooseDialogueResponderIntentState::prepared);
    assert(bound.intent->submitted_task_id.empty());
    const auto disposition = responder_store.find_disposition_for_event(
        "system-responder-v1", event.event_id);
    assert(disposition);
    assert(disposition->kind
        == LocalGooseDialogueResponderDispositionKind::intervention);
    assert(disposition->state
        == LocalGooseDialogueResponderDispositionState::intervention_bound);
    assert(disposition->intent_id == bound.decision->intent_id);

    // Binding is not dispatch: no trigger ACK, no head movement and no lease turn.
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));
    assert(thread_store.find("main")->revision == 1);
    assert(responder_store.find_lease(lease.lease_id)->turns_committed == 0);

    now = 5350;
    const auto bind_retry = service.bind_decision(decision.decision_id);
    assert(bind_retry.result
        == LocalGooseDialogueResponderAuthorityServiceCode::duplicate);
    assert(bind_retry.intent
        && bind_retry.intent->intent_id == bound.intent->intent_id);
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));

    // Restart never auto-dispatches an intent-bound decision.
    LocalGooseDialogueResponderAuthorityService restarted_after_bind(
        completion_store, thread_store, responder_store, authority_store,
        dispatcher, enabled_config, [&now] { return now; });
    const auto bound_status = restarted_after_bind.inspect("main");
    assert(bound_status.event_decision
        && bound_status.event_decision->state
            == LocalGooseDialogueResponderDecisionState::intent_bound);
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));
    assert(thread_store.find("main")->revision == 1);
    assert(responder_store.find_lease(lease.lease_id)->turns_committed == 0);

    assert(disabled.bind_decision(decision.decision_id).result
        == LocalGooseDialogueResponderAuthorityServiceCode::disabled);
    assert(!completion_store.consumer_last_sequence("system-responder-v1"));

    std::cout << "local_goose_dialogue_responder_authority_service_test: PASS\n";
    return 0;
}
