#include "LocalGooseDialogueResponderRetryEligibility.hpp"

#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryEligibilityCode;
using Status = LocalGooseDialogueResponderRetryEligibilityStatus;

Status status(const Code code, std::string reason)
{
    return {code, std::move(reason)};
}

std::string historical_submission_key(
    const LocalGooseDialogueResponderIntent& intent)
{
    return std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(intent.request_id);
}

bool same_event(const LocalGooseDialogueResponderRetryLineage& lineage,
                const LocalGooseDialogueCompletionEvent& event)
{
    return event.sequence == lineage.completion_sequence
        && event.event_id == lineage.event_id
        && event.result_sha256 == lineage.result_sha256
        && event.thread_alias == lineage.thread_alias
        && event.thread_revision == lineage.thread_revision
        && event.task_id == lineage.task_id
        && event.speaker_kind == lineage.observed_speaker_kind
        && event.speaker_id == lineage.observed_speaker_id;
}

bool same_event(const LocalGooseDialogueResponderDecision& decision,
                const LocalGooseDialogueCompletionEvent& event)
{
    return decision.completion_sequence == event.sequence
        && decision.event_id == event.event_id
        && decision.result_sha256 == event.result_sha256
        && decision.thread_alias == event.thread_alias
        && decision.thread_revision == event.thread_revision
        && decision.task_id == event.task_id
        && decision.observed_speaker_kind == event.speaker_kind
        && decision.observed_speaker_id == event.speaker_id;
}

bool same_event(const LocalGooseDialogueResponderDisposition& disposition,
                const LocalGooseDialogueCompletionEvent& event)
{
    return disposition.completion_sequence == event.sequence
        && disposition.event_id == event.event_id
        && disposition.result_sha256 == event.result_sha256
        && disposition.thread_alias == event.thread_alias
        && disposition.thread_revision == event.thread_revision
        && disposition.task_id == event.task_id
        && disposition.speaker_kind == event.speaker_kind
        && disposition.speaker_id == event.speaker_id;
}

bool same_event(const LocalGooseDialogueResponderIntent& intent,
                const LocalGooseDialogueCompletionEvent& event)
{
    return intent.completion_sequence == event.sequence
        && intent.event_id == event.event_id
        && intent.result_sha256 == event.result_sha256
        && intent.thread_alias == event.thread_alias
        && intent.triggering_task_id == event.task_id
        && intent.expected_thread_revision == event.thread_revision
        && intent.expected_head_task_id == event.task_id;
}

} // namespace

LocalGooseDialogueResponderRetryEligibilityService::
LocalGooseDialogueResponderRetryEligibilityService(
    const LocalGooseDialogueCompletionFeedStore& completion,
    const LocalGooseDialogueThreadStore& thread,
    const LocalGooseDialogueResponderStore& responder,
    const LocalGooseDialogueResponderAuthorityStore& authority,
    const gaudere::work::TaskStore& tasks,
    const LocalGooseDialogueResponderRetryStore& retry)
    : completion_(completion),
      thread_(thread),
      responder_(responder),
      authority_(authority),
      tasks_(tasks),
      retry_(retry)
{
}

LocalGooseDialogueResponderRetryEligibilityStatus
LocalGooseDialogueResponderRetryEligibilityService::inspect_generation_1(
    const std::int64_t now_ms) const
{
    using GrantState = LocalGooseDialogueResponderGrantState;
    using LeaseState = LocalGooseDialogueResponderLeaseState;
    using DecisionState = LocalGooseDialogueResponderDecisionState;
    using IntentState = LocalGooseDialogueResponderIntentState;
    using DispositionKind = LocalGooseDialogueResponderDispositionKind;
    using DispositionState = LocalGooseDialogueResponderDispositionState;

    if (now_ms < 0) {
        return status(Code::unavailable, "invalid inspection clock");
    }
    try {
        if (authority_.schema_version()
                != local_goose_dialogue_responder_authority_sidecar_schema
            || retry_.schema_version()
                != local_goose_dialogue_responder_retry_sidecar_schema) {
            return status(Code::unavailable, "unexpected responder schema");
        }

        const auto lineage = retry_.find_lineage(
            local_goose_dialogue_responder_first_retry_lineage_id);
        if (!lineage) {
            return status(Code::lineage_absent, "generation-1 audit lineage absent");
        }
        if (!valid_local_goose_dialogue_responder_retry_lineage(*lineage)
            || lineage->lineage_id
                != local_goose_dialogue_responder_first_retry_lineage_id
            || lineage->consumer_id != "system-responder-v1"
            || lineage->retry_generation != 1
            || lineage->completion_sequence != 4
            || lineage->thread_alias != "main"
            || lineage->thread_revision != 3
            || lineage->observed_speaker_kind != "human"
            || lineage->observed_speaker_id != "bertrand"
            || lineage->historical_grant_terminal_reason != ""
            || lineage->historical_lease_terminal_reason != "lease_expired"
            || lineage->historical_decision_terminal_reason
                != "bound_intent_expired_before_dispatch"
            || lineage->historical_intent_terminal_reason
                != "lease_or_intent_expired"
            || lineage->historical_disposition_terminal_reason
                != "lease_or_intent_expired") {
            return status(Code::conflict, "retry audit lineage differs");
        }
        const auto for_event = retry_.find_for_event(
            lineage->consumer_id, lineage->event_id);
        const auto for_request = retry_.find_by_request(lineage->request_id);
        if (!for_event || !for_request
            || for_event->lineage_id != lineage->lineage_id
            || for_request->lineage_id != lineage->lineage_id) {
            return status(Code::conflict, "retry audit index differs");
        }

        const auto cursor = completion_.consumer_last_sequence(
            lineage->consumer_id);
        const auto event = completion_.next_for_consumer(lineage->consumer_id);
        if (cursor != std::optional<std::uint64_t>{3}
            || !event || !same_event(*lineage, *event)
            || event->speaker_kind != "human") {
            return status(Code::conflict, "held human completion differs");
        }
        const auto head = thread_.find(event->thread_alias);
        if (!head || head->revision != event->thread_revision
            || head->head_task_id != event->task_id
            || !tasks_.find(event->task_id)) {
            return status(Code::conflict, "preferred head or source Task differs");
        }

        const auto grant = authority_.find_grant(lineage->historical_grant_id);
        const auto lease = responder_.find_lease(lineage->historical_lease_id);
        const auto decision = authority_.find_decision(
            lineage->historical_decision_id);
        const auto intent = responder_.find_intent(
            lineage->historical_intent_id);
        const auto disposition = responder_.find_disposition(
            lineage->historical_disposition_id);
        if (!grant || !lease || !decision || !intent || !disposition) {
            return status(Code::conflict, "historical chain incomplete");
        }
        const auto indexed_decision = authority_.find_decision_for_event(
            lineage->consumer_id, lineage->event_id);
        const auto indexed_disposition = responder_.find_disposition_for_event(
            lineage->consumer_id, lineage->event_id);
        if (!indexed_decision || !indexed_disposition
            || indexed_decision->decision_id != decision->decision_id
            || indexed_disposition->disposition_id != disposition->disposition_id) {
            return status(Code::conflict, "historical event ownership differs");
        }

        if (grant->grant_id != lineage->historical_grant_id
            || grant->state != GrantState::activated
            || grant->activated_lease_id
                != std::optional<std::string>{lineage->historical_lease_id}
            || !grant->activated_at_ms
            || grant->expires_at_ms > now_ms
            || !grant->terminal_reason.empty()
            || grant->terminal_at_ms
            || grant->terminal_reason
                != lineage->historical_grant_terminal_reason
            || grant->thread_alias != event->thread_alias
            || grant->max_system_turns != lease->max_system_turns) {
            return status(Code::conflict, "historical grant differs");
        }

        const auto kind = local_goose_dialogue_responder_message_kind_bit(
            decision->message_kind);
        if (lease->lease_id != lineage->historical_lease_id
            || lease->state != LeaseState::expired
            || lease->terminal_reason != lineage->historical_lease_terminal_reason
            || !lease->terminal_at_ms
            || lease->turns_committed != 0
            || lease->expires_at_ms > now_ms
            || lease->thread_alias != event->thread_alias
            || lease->speaker_kind != "system"
            || kind == 0
            || (lease->allowed_message_kinds & kind) == 0) {
            return status(Code::conflict, "historical lease differs");
        }

        if (decision->decision_id != lineage->historical_decision_id
            || decision->consumer_id != lineage->consumer_id
            || decision->grant_id != grant->grant_id
            || decision->lease_id != lease->lease_id
            || decision->state != DecisionState::expired
            || decision->terminal_reason
                != lineage->historical_decision_terminal_reason
            || !decision->terminal_at_ms
            || decision->completed_at_ms
            || decision->intent_id
                != std::optional<std::string>{lineage->historical_intent_id}
            || !decision->bound_at_ms
            || !same_event(*decision, *event)
            || decision->expected_head_revision != head->revision
            || decision->expected_head_task_id != head->head_task_id
            || decision->responder_speaker_kind != lease->speaker_kind
            || decision->responder_speaker_id != lease->speaker_id
            || decision->authority_source != grant->authority_source
            || (grant->allowed_message_kinds & kind) == 0) {
            return status(Code::conflict, "historical decision differs");
        }

        if (intent->intent_id != lineage->historical_intent_id
            || intent->lease_id != lease->lease_id
            || intent->state != IntentState::expired
            || intent->terminal_reason
                != lineage->historical_intent_terminal_reason
            || !same_event(*intent, *event)
            || intent->speaker_kind != decision->responder_speaker_kind
            || intent->speaker_id != decision->responder_speaker_id
            || intent->message_kind != decision->message_kind
            || intent->message != decision->message
            || !intent->submitted_task_id.empty()
            || intent->submitted_thread_revision
            || intent->committed_at_ms
            || intent->completed_at_ms
            || tasks_.find_by_idempotency_key(historical_submission_key(*intent))) {
            return status(Code::conflict, "historical intent or submission differs");
        }

        if (disposition->disposition_id
                != lineage->historical_disposition_id
            || disposition->consumer_id != lineage->consumer_id
            || disposition->kind != DispositionKind::intervention
            || disposition->state != DispositionState::manual_review
            || disposition->terminal_reason
                != lineage->historical_disposition_terminal_reason
            || !disposition->terminal_at_ms
            || !disposition->bound_at_ms
            || disposition->intent_id
                != std::optional<std::string>{lineage->historical_intent_id}
            || !same_event(*disposition, *event)) {
            return status(Code::conflict, "historical disposition differs");
        }

        return status(
            Code::evidence_consistent_no_authority,
            "generation-1 retry audit is consistent; new authority is absent");
    } catch (const std::exception&) {
        return status(Code::unavailable, "authoritative read unavailable");
    }
}

} // namespace gaudere_agent
