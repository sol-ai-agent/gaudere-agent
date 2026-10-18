#include "LocalGooseDialogueResponderAuthorityService.hpp"

#include "Sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr std::size_t max_identifier_bytes = 128;
constexpr const char* deterministic_lease_prefix = "authority-lease-v1-";

bool safe_identifier(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_identifier_bytes) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_'
            || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

LocalGooseDialogueResponderAuthorityServiceCode map_store_result(
    const LocalGooseDialogueResponderStoreResult result) noexcept
{
    switch (result) {
    case LocalGooseDialogueResponderStoreResult::accepted:
        return LocalGooseDialogueResponderAuthorityServiceCode::accepted;
    case LocalGooseDialogueResponderStoreResult::duplicate:
        return LocalGooseDialogueResponderAuthorityServiceCode::duplicate;
    case LocalGooseDialogueResponderStoreResult::conflict:
        return LocalGooseDialogueResponderAuthorityServiceCode::conflict;
    case LocalGooseDialogueResponderStoreResult::invalid:
        return LocalGooseDialogueResponderAuthorityServiceCode::invalid;
    case LocalGooseDialogueResponderStoreResult::unavailable:
        return LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
    }
    return LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
}

LocalGooseDialogueResponderAuthorityServiceCode map_dispatch_result(
    const LocalGooseDialogueResponderDispatchCode result) noexcept
{
    switch (result) {
    case LocalGooseDialogueResponderDispatchCode::accepted:
        return LocalGooseDialogueResponderAuthorityServiceCode::accepted;
    case LocalGooseDialogueResponderDispatchCode::duplicate:
        return LocalGooseDialogueResponderAuthorityServiceCode::duplicate;
    case LocalGooseDialogueResponderDispatchCode::conflict:
        return LocalGooseDialogueResponderAuthorityServiceCode::conflict;
    case LocalGooseDialogueResponderDispatchCode::invalid:
        return LocalGooseDialogueResponderAuthorityServiceCode::invalid;
    case LocalGooseDialogueResponderDispatchCode::unavailable:
        return LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
    }
    return LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
}

std::string deterministic_lease_id(const std::string& grant_id)
{
    return std::string{deterministic_lease_prefix} + sha256_hex(grant_id);
}

bool exact_event_request(
    const LocalGooseDialogueResponderDecisionPrepareRequest& request,
    const LocalGooseDialogueCompletionEvent& event) noexcept
{
    return request.completion_sequence == event.sequence
        && request.event_id == event.event_id
        && request.result_sha256 == event.result_sha256
        && request.thread_alias == event.thread_alias
        && request.thread_revision == event.thread_revision
        && request.task_id == event.task_id
        && request.observed_speaker_kind == event.speaker_kind
        && request.observed_speaker_id == event.speaker_id;
}

bool exact_decision_event(
    const LocalGooseDialogueResponderDecision& decision,
    const LocalGooseDialogueCompletionEvent& event) noexcept
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

bool current_head_matches(
    const LocalGooseDialogueResponderDecisionPrepareRequest& request,
    const LocalGooseDialogueThreadHead& head) noexcept
{
    return request.thread_alias == head.alias
        && request.expected_head_revision == head.revision
        && request.expected_head_task_id == head.head_task_id
        && request.thread_revision == head.revision
        && request.task_id == head.head_task_id;
}

bool current_head_matches(
    const LocalGooseDialogueResponderDecision& decision,
    const LocalGooseDialogueThreadHead& head) noexcept
{
    return decision.thread_alias == head.alias
        && decision.expected_head_revision == head.revision
        && decision.expected_head_task_id == head.head_task_id
        && decision.thread_revision == head.revision
        && decision.task_id == head.head_task_id;
}

bool lease_eligible_now(
    const LocalGooseDialogueResponderLease& lease,
    const std::int64_t now) noexcept
{
    return lease.state == LocalGooseDialogueResponderLeaseState::active
        && now >= lease.issued_at_ms
        && now < lease.expires_at_ms
        && now >= lease.next_eligible_at_ms
        && lease.turns_committed < lease.max_system_turns;
}

bool lease_matches_grant(
    const LocalGooseDialogueResponderLease& lease,
    const LocalGooseDialogueResponderGrant& grant) noexcept
{
    return lease.thread_alias == grant.thread_alias
        && lease.speaker_kind == grant.speaker_kind
        && lease.speaker_id == grant.speaker_id
        && lease.allowed_message_kinds == grant.allowed_message_kinds
        && lease.purpose == grant.purpose
        && lease.max_system_turns == grant.max_system_turns
        && lease.min_interval_ms == grant.min_interval_ms
        && lease.issued_at_ms >= grant.created_at_ms
        && lease.expires_at_ms > lease.issued_at_ms
        && lease.expires_at_ms <= grant.expires_at_ms
        && lease.expires_at_ms - lease.issued_at_ms <= grant.max_lease_ttl_ms;
}

LocalGooseDialogueResponderGrantServiceResult disabled_grant_result()
{
    LocalGooseDialogueResponderGrantServiceResult out;
    out.result = LocalGooseDialogueResponderAuthorityServiceCode::disabled;
    out.detail = "dialogue responder decision authority is disabled";
    return out;
}

LocalGooseDialogueResponderDecisionServiceResult disabled_decision_result()
{
    LocalGooseDialogueResponderDecisionServiceResult out;
    out.result = LocalGooseDialogueResponderAuthorityServiceCode::disabled;
    out.detail = "dialogue responder decision authority is disabled";
    return out;
}

} // namespace

LocalGooseDialogueResponderAuthorityService::
LocalGooseDialogueResponderAuthorityService(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store,
    LocalGooseDialogueResponderDispatcher& dispatcher,
    LocalGooseDialogueResponderAuthorityServiceConfig config,
    LocalGooseDialogueResponderAuthorityClock clock)
    : completion_store_(completion_store),
      thread_store_(thread_store),
      responder_store_(responder_store),
      authority_store_(authority_store),
      dispatcher_(dispatcher),
      config_(std::move(config)),
      clock_(std::move(clock))
{
    if (!safe_identifier(config_.authority_source)
        || !safe_identifier(config_.consumer_id)
        || dispatcher_.consumer_id() != config_.consumer_id
        || !clock_) {
        throw std::invalid_argument(
            "dialogue responder authority service configuration is invalid");
    }
}

LocalGooseDialogueResponderAuthorityStatus
LocalGooseDialogueResponderAuthorityService::inspect(
    const std::string& thread_alias) const
{
    LocalGooseDialogueResponderAuthorityStatus out;
    out.authority_enabled = config_.authority_enabled;
    out.authority_source = config_.authority_source;
    out.consumer_id = config_.consumer_id;
    out.consumer_last_sequence =
        completion_store_.consumer_last_sequence(config_.consumer_id);
    out.next_event = completion_store_.next_for_consumer(config_.consumer_id);
    out.head = thread_store_.find(thread_alias);
    out.active_lease = responder_store_.find_active_lease(thread_alias);

    for (const auto& grant : authority_store_.grants_in_state(
             LocalGooseDialogueResponderGrantState::activated, 1024)) {
        if (grant.thread_alias == thread_alias) {
            out.activated_grant = grant;
            break;
        }
    }
    for (const auto& grant : authority_store_.grants_in_state(
             LocalGooseDialogueResponderGrantState::prepared, 1024)) {
        if (grant.thread_alias == thread_alias) {
            out.prepared_grant = grant;
            break;
        }
    }
    if (out.next_event) {
        out.event_decision = authority_store_.find_decision_for_event(
            config_.consumer_id, out.next_event->event_id);
    }

    if (!out.next_event) {
        out.detail = "no pending responder completion event";
    } else if (out.next_event->speaker_kind != "human") {
        out.detail = "next responder event is not human-decidable";
    } else if (!out.head
               || out.next_event->thread_alias != out.head->alias
               || out.next_event->thread_revision != out.head->revision
               || out.next_event->task_id != out.head->head_task_id) {
        out.detail = "pending human event is not the current preferred head";
    } else if (out.event_decision) {
        out.detail = "current human event already has a durable decision";
    } else if (!out.activated_grant) {
        out.detail = out.prepared_grant
            ? "current human event held; grant is prepared but not activated"
            : "current human event held; no activated authority grant";
    } else if (!out.active_lease) {
        out.detail = "current human event held; activated grant has no active lease";
    } else {
        out.detail = "current human event held; explicit decision is required";
    }
    return out;
}

LocalGooseDialogueResponderGrantServiceResult
LocalGooseDialogueResponderAuthorityService::prepare_grant(
    const LocalGooseDialogueResponderGrantPrepareRequest& request)
{
    if (!config_.authority_enabled) return disabled_grant_result();

    LocalGooseDialogueResponderGrantServiceResult out;
    if (authority_store_.schema_version()
        != local_goose_dialogue_responder_authority_sidecar_schema) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority clock is invalid";
        return out;
    }
    const auto grant = make_local_goose_dialogue_responder_grant(
        request.request_id,
        config_.authority_source,
        request.thread_alias,
        request.speaker_id,
        request.allowed_message_kinds,
        request.purpose,
        request.max_system_turns,
        request.max_lease_ttl_ms,
        request.min_interval_ms,
        now,
        request.grant_ttl_ms);
    const auto write = authority_store_.prepare_grant(grant);
    out.result = map_store_result(write.result);
    out.grant = write.grant;
    out.detail = write.detail;
    return out;
}

LocalGooseDialogueResponderGrantServiceResult
LocalGooseDialogueResponderAuthorityService::activate_grant(
    const std::string& grant_id)
{
    if (!config_.authority_enabled) return disabled_grant_result();

    LocalGooseDialogueResponderGrantServiceResult out;
    if (authority_store_.schema_version()
        != local_goose_dialogue_responder_authority_sidecar_schema) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    auto grant = authority_store_.find_grant(grant_id);
    if (!grant) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder authority grant not found";
        return out;
    }
    out.grant = grant;

    if (grant->state == LocalGooseDialogueResponderGrantState::activated) {
        if (!grant->activated_lease_id) {
            out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
            out.detail = "activated grant has no durable lease identity";
            return out;
        }
        out.lease = responder_store_.find_lease(*grant->activated_lease_id);
        if (!out.lease || !lease_matches_grant(*out.lease, *grant)) {
            out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
            out.detail = "activated grant lease evidence conflicts";
            return out;
        }
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::duplicate;
        out.detail = "responder authority grant already activated";
        return out;
    }
    if (grant->state != LocalGooseDialogueResponderGrantState::prepared) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder authority grant is not activatable";
        return out;
    }

    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority clock is invalid";
        return out;
    }
    if (now >= grant->expires_at_ms) {
        const auto expired = authority_store_.terminalize_grant(
            grant->grant_id,
            LocalGooseDialogueResponderGrantState::expired,
            "grant_expired",
            now);
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.grant = expired.grant ? expired.grant : grant;
        out.detail = "responder authority grant expired before activation";
        return out;
    }

    const auto lease_id = deterministic_lease_id(grant->grant_id);
    auto lease = responder_store_.find_lease(lease_id);
    if (!lease) {
        const auto remaining = grant->expires_at_ms - now;
        const auto lease_ttl = std::min(grant->max_lease_ttl_ms, remaining);
        if (lease_ttl <= 0
            || now > std::numeric_limits<std::int64_t>::max() - lease_ttl) {
            out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
            out.detail = "responder grant has no remaining lease lifetime";
            return out;
        }
        LocalGooseDialogueResponderLease candidate;
        candidate.lease_id = lease_id;
        candidate.thread_alias = grant->thread_alias;
        candidate.speaker_kind = grant->speaker_kind;
        candidate.speaker_id = grant->speaker_id;
        candidate.allowed_message_kinds = grant->allowed_message_kinds;
        candidate.purpose = grant->purpose;
        candidate.max_system_turns = grant->max_system_turns;
        candidate.issued_at_ms = now;
        candidate.expires_at_ms = now + lease_ttl;
        candidate.min_interval_ms = grant->min_interval_ms;
        candidate.next_eligible_at_ms = now;
        const auto created = responder_store_.create_lease(candidate);
        if ((created.result != LocalGooseDialogueResponderStoreResult::accepted
             && created.result != LocalGooseDialogueResponderStoreResult::duplicate)
            || !created.lease) {
            out.result = map_store_result(created.result);
            out.lease = created.lease;
            out.detail = "responder grant lease creation failed: " + created.detail;
            return out;
        }
        lease = created.lease;
    }
    out.lease = lease;
    if (!lease || lease->lease_id != lease_id
        || !lease_matches_grant(*lease, *grant)
        || lease->turns_committed != 0) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "deterministic responder grant lease evidence conflicts";
        return out;
    }

    const auto linked = authority_store_.activate_grant(
        grant->grant_id, lease_id, lease->issued_at_ms);
    out.result = map_store_result(linked.result);
    out.grant = linked.grant;
    out.detail = linked.detail;
    return out;
}

LocalGooseDialogueResponderDecisionServiceResult
LocalGooseDialogueResponderAuthorityService::prepare_decision(
    const LocalGooseDialogueResponderDecisionPrepareRequest& request)
{
    if (!config_.authority_enabled) return disabled_decision_result();

    LocalGooseDialogueResponderDecisionServiceResult out;
    if (authority_store_.schema_version()
        != local_goose_dialogue_responder_authority_sidecar_schema) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    if (request.consumer_id != config_.consumer_id) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder decision consumer identity differs";
        return out;
    }
    const auto event = completion_store_.next_for_consumer(config_.consumer_id);
    const auto head = thread_store_.find(request.thread_alias);
    if (!event || !head || !exact_event_request(request, *event)
        || !current_head_matches(request, *head)
        || event->speaker_kind != "human") {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder decision exact completion/head evidence differs";
        return out;
    }

    const auto grant = authority_store_.find_grant(request.grant_id);
    const auto lease = responder_store_.find_lease(request.lease_id);
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority clock is invalid";
        return out;
    }
    if (!grant
        || grant->state != LocalGooseDialogueResponderGrantState::activated
        || grant->activated_lease_id
            != std::optional<std::string>{request.lease_id}
        || grant->authority_source != config_.authority_source
        || !lease || !lease_eligible_now(*lease, now)
        || !lease_matches_grant(*lease, *grant)) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder decision grant/lease authority is not eligible";
        return out;
    }

    const auto decision = make_local_goose_dialogue_responder_decision(
        request.request_id,
        config_.authority_source,
        grant->grant_id,
        lease->lease_id,
        config_.consumer_id,
        event->sequence,
        event->event_id,
        event->result_sha256,
        event->thread_alias,
        event->thread_revision,
        event->task_id,
        event->speaker_kind,
        event->speaker_id,
        head->revision,
        head->head_task_id,
        lease->speaker_id,
        request.message_kind,
        request.message,
        request.intent_ttl_ms,
        now);
    const auto write = authority_store_.prepare_decision(decision);
    out.result = map_store_result(write.result);
    out.decision = write.decision;
    out.detail = write.detail;
    return out;
}

LocalGooseDialogueResponderDecisionServiceResult
LocalGooseDialogueResponderAuthorityService::bind_decision(
    const std::string& decision_id)
{
    if (!config_.authority_enabled) return disabled_decision_result();

    LocalGooseDialogueResponderDecisionServiceResult out;
    if (authority_store_.schema_version()
        != local_goose_dialogue_responder_authority_sidecar_schema) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    auto decision = authority_store_.find_decision(decision_id);
    if (!decision) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder decision not found";
        return out;
    }
    out.decision = decision;
    if (decision->state == LocalGooseDialogueResponderDecisionState::intent_bound) {
        if (!decision->intent_id) {
            out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
            out.detail = "intent-bound decision has no intent identity";
            return out;
        }
        out.intent = responder_store_.find_intent(*decision->intent_id);
        if (!out.intent) {
            out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
            out.detail = "intent-bound decision durable intent is missing";
            return out;
        }
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::duplicate;
        out.detail = "responder decision already bound to intent";
        return out;
    }
    if (decision->state != LocalGooseDialogueResponderDecisionState::prepared) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.detail = "responder decision is not bindable";
        return out;
    }

    const auto event = completion_store_.next_for_consumer(config_.consumer_id);
    const auto head = thread_store_.find(decision->thread_alias);
    const auto grant = authority_store_.find_grant(decision->grant_id);
    const auto lease = responder_store_.find_lease(decision->lease_id);
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::unavailable;
        out.detail = "responder authority clock is invalid";
        return out;
    }
    if (!event || !head || !exact_decision_event(*decision, *event)
        || event->speaker_kind != "human"
        || !current_head_matches(*decision, *head)
        || !grant
        || grant->state != LocalGooseDialogueResponderGrantState::activated
        || grant->activated_lease_id
            != std::optional<std::string>{decision->lease_id}
        || grant->authority_source != config_.authority_source
        || !lease || !lease_eligible_now(*lease, now)
        || !lease_matches_grant(*lease, *grant)) {
        const auto terminal = authority_store_.terminalize_decision(
            decision->decision_id,
            LocalGooseDialogueResponderDecisionState::conflict,
            "canonical_evidence_or_authority_changed",
            now);
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.decision = terminal.decision ? terminal.decision : decision;
        out.detail = "responder decision became stale before intent admission";
        return out;
    }

    const auto prepared = dispatcher_.prepare(
        decision->lease_id,
        decision->completion_sequence,
        decision->message_kind,
        decision->message,
        decision->intent_ttl_ms);
    out.intent = prepared.intent;
    if ((prepared.result != LocalGooseDialogueResponderDispatchCode::accepted
         && prepared.result != LocalGooseDialogueResponderDispatchCode::duplicate)
        || !prepared.intent) {
        out.result = map_dispatch_result(prepared.result);
        out.detail = "responder intent admission failed: " + prepared.detail;
        if (prepared.result == LocalGooseDialogueResponderDispatchCode::conflict) {
            const auto terminal = authority_store_.terminalize_decision(
                decision->decision_id,
                LocalGooseDialogueResponderDecisionState::conflict,
                "intent_admission_conflict",
                now);
            if (terminal.decision) out.decision = terminal.decision;
        }
        return out;
    }

    const auto linked = authority_store_.bind_decision_intent(
        decision->decision_id,
        prepared.intent->intent_id,
        now);
    if (linked.result == LocalGooseDialogueResponderStoreResult::accepted
        || linked.result == LocalGooseDialogueResponderStoreResult::duplicate) {
        out.result =
            prepared.result == LocalGooseDialogueResponderDispatchCode::accepted
                || linked.result == LocalGooseDialogueResponderStoreResult::accepted
            ? LocalGooseDialogueResponderAuthorityServiceCode::accepted
            : LocalGooseDialogueResponderAuthorityServiceCode::duplicate;
        out.decision = linked.decision;
        out.detail = linked.detail;
        return out;
    }

    if (linked.result == LocalGooseDialogueResponderStoreResult::conflict) {
        const auto review = authority_store_.terminalize_decision(
            decision->decision_id,
            LocalGooseDialogueResponderDecisionState::manual_review,
            "intent_binding_reconciliation_conflict",
            now);
        out.result = LocalGooseDialogueResponderAuthorityServiceCode::conflict;
        out.decision = review.decision ? review.decision : decision;
        out.detail = "responder intent exists but decision binding needs manual review";
        return out;
    }

    out.result = map_store_result(linked.result);
    out.decision = linked.decision;
    out.detail = linked.detail;
    return out;
}

} // namespace gaudere_agent
