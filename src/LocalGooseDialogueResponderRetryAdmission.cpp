#include "LocalGooseDialogueResponderRetryAdmission.hpp"

#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr const char* lease_expired_reason = "lease_expired";
constexpr const char* failed_attempt_reason = "lease_or_intent_expired";
constexpr const char* decision_expired_reason =
    "bound_intent_expired_before_dispatch";

LocalGooseDialogueResponderRetryAdmissionWrite reject(
    const LocalGooseDialogueResponderRetryAdmissionResult result,
    std::string detail)
{
    LocalGooseDialogueResponderRetryAdmissionWrite out;
    out.result = result;
    out.detail = std::move(detail);
    return out;
}

std::string submission_idempotency_key(
    const LocalGooseDialogueResponderIntent& intent)
{
    return std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(intent.request_id);
}

LocalGooseDialogueResponderRetryAdmissionResult map_store_result(
    const LocalGooseDialogueResponderRetryStoreResult result)
{
    using Admission = LocalGooseDialogueResponderRetryAdmissionResult;
    using Store = LocalGooseDialogueResponderRetryStoreResult;
    switch (result) {
    case Store::accepted: return Admission::accepted;
    case Store::duplicate: return Admission::duplicate;
    case Store::conflict: return Admission::conflict;
    case Store::invalid: return Admission::invalid;
    case Store::unavailable: return Admission::unavailable;
    }
    return Admission::unavailable;
}

bool same_trigger(
    const LocalGooseDialogueResponderDecision& decision,
    const LocalGooseDialogueCompletionEvent& event) noexcept
{
    return decision.consumer_id.size() > 0
        && decision.completion_sequence == event.sequence
        && decision.event_id == event.event_id
        && decision.result_sha256 == event.result_sha256
        && decision.thread_alias == event.thread_alias
        && decision.thread_revision == event.thread_revision
        && decision.task_id == event.task_id
        && decision.observed_speaker_kind == event.speaker_kind
        && decision.observed_speaker_id == event.speaker_id;
}

bool same_trigger(
    const LocalGooseDialogueResponderIntent& intent,
    const LocalGooseDialogueCompletionEvent& event) noexcept
{
    return intent.completion_sequence == event.sequence
        && intent.event_id == event.event_id
        && intent.result_sha256 == event.result_sha256
        && intent.thread_alias == event.thread_alias
        && intent.triggering_task_id == event.task_id;
}

bool same_trigger(
    const LocalGooseDialogueResponderDisposition& disposition,
    const LocalGooseDialogueCompletionEvent& event) noexcept
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

} // namespace

LocalGooseDialogueResponderRetryAdmissionService::
LocalGooseDialogueResponderRetryAdmissionService(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store,
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueResponderRetryStore& retry_store,
    LocalGooseDialogueResponderRetryAdmissionClock clock)
    : completion_store_(completion_store),
      thread_store_(thread_store),
      responder_store_(responder_store),
      authority_store_(authority_store),
      task_store_(task_store),
      retry_store_(retry_store),
      clock_(std::move(clock))
{
    if (!clock_) {
        throw std::invalid_argument("retry admission clock is required");
    }
}

LocalGooseDialogueResponderRetryAdmissionWrite
LocalGooseDialogueResponderRetryAdmissionService::prepare_lineage(
    const LocalGooseDialogueResponderRetryAdmissionRequest& request)
{
    using Admission = LocalGooseDialogueResponderRetryAdmissionResult;
    using GrantState = LocalGooseDialogueResponderGrantState;
    using LeaseState = LocalGooseDialogueResponderLeaseState;
    using DecisionState = LocalGooseDialogueResponderDecisionState;
    using IntentState = LocalGooseDialogueResponderIntentState;
    using DispositionKind = LocalGooseDialogueResponderDispositionKind;
    using DispositionState = LocalGooseDialogueResponderDispositionState;

    if (request.request_id.empty() || request.consumer_id.empty()
        || request.historical_grant_id.empty()
        || request.historical_lease_id.empty()
        || request.historical_decision_id.empty()
        || request.historical_intent_id.empty()
        || request.historical_disposition_id.empty()) {
        return reject(Admission::invalid, "retry admission request is incomplete");
    }

    try {
        if (authority_store_.schema_version()
                != local_goose_dialogue_responder_authority_sidecar_schema
            || retry_store_.schema_version()
                != local_goose_dialogue_responder_retry_sidecar_schema) {
            return reject(
                Admission::unavailable,
                "retry admission requires authority schema v3 and retry schema v1");
        }

        const auto now = clock_();
        if (now < 0) {
            return reject(Admission::unavailable, "retry admission clock failed");
        }

        const auto grant = authority_store_.find_grant(request.historical_grant_id);
        const auto lease = responder_store_.find_lease(request.historical_lease_id);
        const auto decision = authority_store_.find_decision(
            request.historical_decision_id);
        const auto intent = responder_store_.find_intent(request.historical_intent_id);
        const auto disposition = responder_store_.find_disposition(
            request.historical_disposition_id);
        if (!grant || !lease || !decision || !intent || !disposition) {
            return reject(
                Admission::conflict,
                "historical responder authority chain is incomplete");
        }

        const auto event = completion_store_.next_for_consumer(request.consumer_id);
        if (!event || event->speaker_kind != "human") {
            return reject(
                Admission::conflict,
                "retry trigger is not the current pending human completion");
        }
        const auto head = thread_store_.find(event->thread_alias);
        if (!head || head->revision != event->thread_revision
            || head->head_task_id != event->task_id) {
            return reject(
                Admission::conflict,
                "preferred head no longer matches retry trigger");
        }
        if (!task_store_.find(event->task_id)) {
            return reject(
                Admission::conflict,
                "retry trigger Task is missing from the authoritative Task store");
        }

        if (decision->consumer_id != request.consumer_id
            || decision->grant_id != request.historical_grant_id
            || decision->lease_id != request.historical_lease_id
            || decision->state != DecisionState::expired
            || decision->terminal_reason != decision_expired_reason
            || !decision->terminal_at_ms
            || decision->intent_id
                != std::optional<std::string>{request.historical_intent_id}
            || !decision->bound_at_ms
            || decision->completed_at_ms
            || !same_trigger(*decision, *event)
            || decision->expected_head_revision != head->revision
            || decision->expected_head_task_id != head->head_task_id) {
            return reject(
                Admission::conflict,
                "historical decision does not match recovered retry evidence");
        }

        if (intent->lease_id != request.historical_lease_id
            || intent->state != IntentState::expired
            || intent->terminal_reason != failed_attempt_reason
            || !same_trigger(*intent, *event)
            || intent->expected_thread_revision != head->revision
            || intent->expected_head_task_id != head->head_task_id
            || intent->speaker_kind != decision->responder_speaker_kind
            || intent->speaker_id != decision->responder_speaker_id
            || intent->message_kind != decision->message_kind
            || intent->message != decision->message
            || !intent->submitted_task_id.empty()
            || intent->submitted_thread_revision
            || intent->committed_at_ms
            || intent->completed_at_ms) {
            return reject(
                Admission::conflict,
                "historical intent does not match recovered retry evidence");
        }

        if (disposition->disposition_id != request.historical_disposition_id
            || disposition->consumer_id != request.consumer_id
            || disposition->kind != DispositionKind::intervention
            || disposition->state != DispositionState::manual_review
            || disposition->terminal_reason != failed_attempt_reason
            || !disposition->terminal_at_ms
            || disposition->intent_id
                != std::optional<std::string>{request.historical_intent_id}
            || !disposition->bound_at_ms
            || !same_trigger(*disposition, *event)) {
            return reject(
                Admission::conflict,
                "historical disposition does not match recovered retry evidence");
        }

        const auto kind_bit = local_goose_dialogue_responder_message_kind_bit(
            decision->message_kind);
        if (lease->lease_id != request.historical_lease_id
            || lease->state != LeaseState::expired
            || lease->terminal_reason != lease_expired_reason
            || !lease->terminal_at_ms
            || lease->turns_committed != 0
            || lease->thread_alias != event->thread_alias
            || lease->speaker_kind != decision->responder_speaker_kind
            || lease->speaker_id != decision->responder_speaker_id
            || kind_bit == 0
            || (lease->allowed_message_kinds & kind_bit) == 0
            || lease->expires_at_ms > now) {
            return reject(
                Admission::conflict,
                "historical lease is not the recovered unused expired lease");
        }

        if (grant->grant_id != request.historical_grant_id
            || grant->state != GrantState::activated
            || grant->activated_lease_id
                != std::optional<std::string>{request.historical_lease_id}
            || !grant->activated_at_ms
            || grant->thread_alias != event->thread_alias
            || grant->speaker_kind != decision->responder_speaker_kind
            || grant->speaker_id != decision->responder_speaker_id
            || grant->authority_source != decision->authority_source
            || (grant->allowed_message_kinds & kind_bit) == 0
            || grant->max_system_turns != lease->max_system_turns
            || grant->expires_at_ms > now
            || !grant->terminal_reason.empty()
            || grant->terminal_at_ms) {
            return reject(
                Admission::conflict,
                "historical grant is not the immutable activated expired-window grant");
        }

        if (task_store_.find_by_idempotency_key(
                submission_idempotency_key(*intent))) {
            return reject(
                Admission::conflict,
                "historical intent already has a durable submission Task");
        }

        auto lineage = make_local_goose_dialogue_responder_retry_lineage(
            request.request_id,
            request.consumer_id,
            event->thread_alias,
            event->sequence,
            event->event_id,
            event->result_sha256,
            event->thread_revision,
            event->task_id,
            event->speaker_id,
            grant->grant_id,
            lease->lease_id,
            decision->decision_id,
            intent->intent_id,
            disposition->disposition_id,
            grant->terminal_reason,
            lease->terminal_reason,
            decision->terminal_reason,
            intent->terminal_reason,
            disposition->terminal_reason,
            now);
        if (!valid_local_goose_dialogue_responder_retry_lineage(lineage)) {
            return reject(
                Admission::invalid,
                "recovered evidence cannot form a canonical retry lineage");
        }

        const auto write = retry_store_.prepare_lineage(lineage);
        LocalGooseDialogueResponderRetryAdmissionWrite out;
        out.result = map_store_result(write.result);
        out.lineage = write.lineage;
        out.detail = write.detail;
        return out;
    } catch (const std::exception& exception) {
        return reject(Admission::unavailable, exception.what());
    }
}

} // namespace gaudere_agent
