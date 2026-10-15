#include "LocalGooseDialogueResponderDispatcher.hpp"

#include "LocalGooseDialogueV3.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr std::int64_t max_intent_ttl_ms = 24LL * 60 * 60 * 1000;
constexpr const char* intervention_authority_source =
    "explicit-intervention-intent-v1";

bool safe_identifier(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_'
            || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool same_trigger(
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueResponderIntent& intent) noexcept
{
    return event.sequence == intent.completion_sequence
        && event.event_id == intent.event_id
        && event.thread_alias == intent.thread_alias
        && event.thread_revision == intent.expected_thread_revision
        && event.task_id == intent.triggering_task_id
        && event.task_id == intent.expected_head_task_id
        && event.result_sha256 == intent.result_sha256;
}

bool same_submitted_task(
    const gaudere::work::Task& task,
    const LocalGooseDialogueResponderIntent& intent) noexcept
{
    const auto inspected = inspect_local_goose_dialogue_v3_task(task);
    return inspected.eligible
        && task.id == intent.submitted_task_id
        && inspected.request_id == intent.request_id
        && inspected.speaker_kind == intent.speaker_kind
        && inspected.speaker_id == intent.speaker_id
        && inspected.message_kind == intent.message_kind
        && inspected.message == intent.message;
}

bool same_intervention_disposition_identity(
    const LocalGooseDialogueResponderDisposition& disposition,
    const LocalGooseDialogueResponderDisposition& candidate) noexcept
{
    return disposition.disposition_id == candidate.disposition_id
        && disposition.consumer_id == candidate.consumer_id
        && disposition.thread_alias == candidate.thread_alias
        && disposition.completion_sequence == candidate.completion_sequence
        && disposition.event_id == candidate.event_id
        && disposition.result_sha256 == candidate.result_sha256
        && disposition.thread_revision == candidate.thread_revision
        && disposition.task_id == candidate.task_id
        && disposition.speaker_kind == candidate.speaker_kind
        && disposition.speaker_id == candidate.speaker_id
        && disposition.kind == candidate.kind
        && disposition.authority_source == candidate.authority_source
        && disposition.created_at_ms == candidate.created_at_ms;
}

bool intervention_disposition_matches_intent(
    const LocalGooseDialogueResponderDisposition& disposition,
    const LocalGooseDialogueResponderIntent& intent,
    const std::string& consumer_id) noexcept
{
    return disposition.consumer_id == consumer_id
        && disposition.thread_alias == intent.thread_alias
        && disposition.completion_sequence == intent.completion_sequence
        && disposition.event_id == intent.event_id
        && disposition.result_sha256 == intent.result_sha256
        && disposition.thread_revision == intent.expected_thread_revision
        && disposition.task_id == intent.triggering_task_id
        && disposition.task_id == intent.expected_head_task_id
        && disposition.speaker_kind == "human"
        && disposition.kind
            == LocalGooseDialogueResponderDispositionKind::intervention
        && disposition.authority_source == intervention_authority_source
        && disposition.intent_id
            == std::optional<std::string>{intent.intent_id}
        && disposition.bound_at_ms.has_value();
}

bool bound_intervention_matches_intent(
    const LocalGooseDialogueResponderDisposition& disposition,
    const LocalGooseDialogueResponderIntent& intent,
    const std::string& consumer_id) noexcept
{
    using State = LocalGooseDialogueResponderDispositionState;
    return intervention_disposition_matches_intent(
               disposition, intent, consumer_id)
        && (disposition.state == State::intervention_bound
            || disposition.state == State::completed);
}

LocalGooseDialogueResponderDispatchCode map_store_result(
    const LocalGooseDialogueResponderStoreResult result) noexcept
{
    switch (result) {
    case LocalGooseDialogueResponderStoreResult::accepted:
        return LocalGooseDialogueResponderDispatchCode::accepted;
    case LocalGooseDialogueResponderStoreResult::duplicate:
        return LocalGooseDialogueResponderDispatchCode::duplicate;
    case LocalGooseDialogueResponderStoreResult::conflict:
        return LocalGooseDialogueResponderDispatchCode::conflict;
    case LocalGooseDialogueResponderStoreResult::invalid:
        return LocalGooseDialogueResponderDispatchCode::invalid;
    case LocalGooseDialogueResponderStoreResult::unavailable:
        return LocalGooseDialogueResponderDispatchCode::unavailable;
    }
    return LocalGooseDialogueResponderDispatchCode::unavailable;
}

LocalGooseDialogueResponderDispatchCode map_submit_result(
    const LocalGooseDialoguePreferredSubmitResultCode result) noexcept
{
    switch (result) {
    case LocalGooseDialoguePreferredSubmitResultCode::accepted:
        return LocalGooseDialogueResponderDispatchCode::accepted;
    case LocalGooseDialoguePreferredSubmitResultCode::duplicate:
        return LocalGooseDialogueResponderDispatchCode::duplicate;
    case LocalGooseDialoguePreferredSubmitResultCode::conflict:
        return LocalGooseDialogueResponderDispatchCode::conflict;
    case LocalGooseDialoguePreferredSubmitResultCode::invalid:
        return LocalGooseDialogueResponderDispatchCode::invalid;
    case LocalGooseDialoguePreferredSubmitResultCode::unavailable:
        return LocalGooseDialogueResponderDispatchCode::unavailable;
    }
    return LocalGooseDialogueResponderDispatchCode::unavailable;
}

struct TerminalInterventionResult {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::unavailable;
    std::optional<LocalGooseDialogueResponderIntent> intent;
    std::optional<LocalGooseDialogueResponderDisposition> disposition;
    std::string detail;
};

bool terminal_intent_state(
    const LocalGooseDialogueResponderIntentState state) noexcept
{
    using State = LocalGooseDialogueResponderIntentState;
    return state == State::conflict
        || state == State::expired
        || state == State::manual_review;
}

TerminalInterventionResult reconcile_terminal_intervention_disposition(
    LocalGooseDialogueResponderStore& responder_store,
    const std::string& consumer_id,
    const LocalGooseDialogueResponderIntent& intent,
    const std::int64_t terminal_at_ms)
{
    TerminalInterventionResult out;
    out.intent = intent;
    if (!terminal_intent_state(intent.state)
        || intent.terminal_reason.empty()
        || terminal_at_ms < 0) {
        out.result = LocalGooseDialogueResponderStoreResult::invalid;
        out.detail = "invalid terminal responder intervention reconciliation";
        return out;
    }

    auto disposition = responder_store.find_disposition_for_event(
        consumer_id, intent.event_id);
    out.disposition = disposition;
    if (!disposition) {
        out.result = LocalGooseDialogueResponderStoreResult::conflict;
        out.detail = "terminal responder intent has no durable disposition";
        return out;
    }
    if (!intervention_disposition_matches_intent(
            *disposition, intent, consumer_id)) {
        out.result = LocalGooseDialogueResponderStoreResult::conflict;
        out.detail = "terminal responder intent disposition conflicts";
        return out;
    }

    using DState = LocalGooseDialogueResponderDispositionState;
    if (disposition->state == DState::manual_review) {
        if (disposition->terminal_reason != intent.terminal_reason) {
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "terminal responder intent/disposition reasons differ";
            return out;
        }
        out.result = LocalGooseDialogueResponderStoreResult::duplicate;
        out.detail = "terminal responder disposition already reconciled";
        return out;
    }
    if (disposition->state != DState::intervention_bound) {
        out.result = LocalGooseDialogueResponderStoreResult::conflict;
        out.detail = "terminal responder disposition is in an incompatible state";
        return out;
    }

    const auto terminal = responder_store.terminalize_disposition(
        disposition->disposition_id,
        intent.terminal_reason,
        terminal_at_ms);
    out.result = terminal.result;
    out.disposition = terminal.disposition;
    out.detail = terminal.detail;
    return out;
}

TerminalInterventionResult terminalize_intervention(
    LocalGooseDialogueResponderStore& responder_store,
    const std::string& consumer_id,
    const LocalGooseDialogueResponderIntent& intent,
    const LocalGooseDialogueResponderIntentState terminal_state,
    const std::string& reason,
    const std::int64_t terminal_at_ms)
{
    TerminalInterventionResult out;
    const auto terminal_intent = responder_store.terminalize_intent(
        intent.intent_id, terminal_state, reason);
    out.result = terminal_intent.result;
    out.intent = terminal_intent.intent;
    out.detail = terminal_intent.detail;
    if ((terminal_intent.result
            != LocalGooseDialogueResponderStoreResult::accepted
         && terminal_intent.result
            != LocalGooseDialogueResponderStoreResult::duplicate)
        || !terminal_intent.intent) {
        return out;
    }

    auto disposition = reconcile_terminal_intervention_disposition(
        responder_store, consumer_id, *terminal_intent.intent,
        terminal_at_ms);
    disposition.intent = terminal_intent.intent;
    if (disposition.result
            != LocalGooseDialogueResponderStoreResult::accepted
        && disposition.result
            != LocalGooseDialogueResponderStoreResult::duplicate) {
        return disposition;
    }
    disposition.result =
        terminal_intent.result == LocalGooseDialogueResponderStoreResult::accepted
            || disposition.result
                == LocalGooseDialogueResponderStoreResult::accepted
        ? LocalGooseDialogueResponderStoreResult::accepted
        : LocalGooseDialogueResponderStoreResult::duplicate;
    disposition.detail = "terminal responder intent/disposition reconciled";
    return disposition;
}

void attach_cursor(
    LocalGooseDialogueResponderDispatchResult& out,
    const LocalGooseDialogueCompletionFeedStore& completion_store,
    const std::string& consumer_id)
{
    const auto cursor = completion_store.consumer_last_sequence(consumer_id);
    if (cursor) out.consumer_last_sequence = *cursor;
}

} // namespace

LocalGooseDialogueResponderDispatcher::LocalGooseDialogueResponderDispatcher(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueResponderStore& responder_store,
    std::string model_sha256,
    std::string consumer_id,
    LocalGooseDialogueResponderClock clock)
    : runtime_(runtime),
      task_store_(task_store),
      thread_store_(thread_store),
      completion_store_(completion_store),
      responder_store_(responder_store),
      model_sha256_(std::move(model_sha256)),
      consumer_id_(std::move(consumer_id)),
      clock_(std::move(clock))
{
    if (model_sha256_.empty() || !safe_identifier(consumer_id_) || !clock_) {
        throw std::invalid_argument(
            "dialogue responder dispatcher requires model, consumer id and clock");
    }
}

const std::string&
LocalGooseDialogueResponderDispatcher::consumer_id() const noexcept
{
    return consumer_id_;
}

LocalGooseDialogueResponderLeaseResult
LocalGooseDialogueResponderDispatcher::create_lease(
    const std::string& lease_id,
    const std::string& thread_alias,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& purpose,
    const std::uint64_t max_system_turns,
    const std::int64_t lease_ttl_ms,
    const std::int64_t min_interval_ms)
{
    LocalGooseDialogueResponderLeaseResult out;
    if (!safe_identifier(lease_id)
        || !safe_identifier(thread_alias)
        || !safe_identifier(speaker_id)
        || local_goose_dialogue_responder_message_kind_bit(message_kind) == 0
        || purpose.empty() || purpose.size() > 1024
        || max_system_turns == 0 || max_system_turns > 8
        || lease_ttl_ms <= 0 || lease_ttl_ms > max_intent_ttl_ms
        || min_interval_ms < 0 || min_interval_ms > max_intent_ttl_ms) {
        out.detail = "invalid dialogue responder lease request";
        return out;
    }
    const auto now = clock_();
    if (now < 0
        || now > std::numeric_limits<std::int64_t>::max() - lease_ttl_ms) {
        out.detail = "dialogue responder lease clock/ttl is invalid";
        return out;
    }

    const auto current_active =
        responder_store_.find_active_lease(thread_alias);
    if (current_active
        && current_active->state == LocalGooseDialogueResponderLeaseState::active
        && now >= current_active->expires_at_ms) {
        const auto expired = responder_store_.close_lease(
            current_active->lease_id,
            LocalGooseDialogueResponderLeaseState::expired,
            "lease_expired", now);
        if (expired.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && expired.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            out.result = map_store_result(expired.result);
            out.lease = expired.lease;
            out.detail =
                "expired responder lease normalization failed: "
                + expired.detail;
            return out;
        }
    }

    LocalGooseDialogueResponderLease lease;
    lease.lease_id = lease_id;
    lease.thread_alias = thread_alias;
    lease.speaker_id = speaker_id;
    lease.allowed_message_kinds =
        local_goose_dialogue_responder_message_kind_bit(message_kind);
    lease.purpose = purpose;
    lease.max_system_turns = max_system_turns;
    lease.issued_at_ms = now;
    lease.expires_at_ms = now + lease_ttl_ms;
    lease.min_interval_ms = min_interval_ms;
    lease.next_eligible_at_ms = now;

    const auto write = responder_store_.create_lease(lease);
    out.result = map_store_result(write.result);
    out.lease = write.lease;
    out.detail = write.detail;
    return out;
}

LocalGooseDialogueResponderLeaseResult
LocalGooseDialogueResponderDispatcher::revoke_lease(
    const std::string& lease_id,
    const std::string& reason)
{
    LocalGooseDialogueResponderLeaseResult out;
    if (!safe_identifier(lease_id)
        || reason.empty() || reason.size() > 1024) {
        out.detail = "invalid dialogue responder lease revocation";
        return out;
    }
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
        out.detail = "dialogue responder clock is invalid";
        return out;
    }
    const auto write = responder_store_.close_lease(
        lease_id, LocalGooseDialogueResponderLeaseState::revoked,
        reason, now);
    out.result = map_store_result(write.result);
    out.lease = write.lease;
    out.detail = write.detail;
    return out;
}

std::optional<LocalGooseDialogueResponderLease>
LocalGooseDialogueResponderDispatcher::find_lease(
    const std::string& lease_id) const
{
    return responder_store_.find_lease(lease_id);
}

std::optional<LocalGooseDialogueResponderIntent>
LocalGooseDialogueResponderDispatcher::find_intent(
    const std::string& intent_id) const
{
    return responder_store_.find_intent(intent_id);
}

LocalGooseDialogueResponderDispatchResult
LocalGooseDialogueResponderDispatcher::prepare(
    const std::string& lease_id,
    const std::uint64_t completion_sequence,
    const std::string& message_kind,
    const std::string& message,
    const std::int64_t intent_ttl_ms)
{
    LocalGooseDialogueResponderDispatchResult out;
    attach_cursor(out, completion_store_, consumer_id_);

    if (!safe_identifier(lease_id)
        || completion_sequence == 0
        || intent_ttl_ms <= 0
        || intent_ttl_ms > max_intent_ttl_ms) {
        out.detail = "invalid dialogue responder prepare request";
        return out;
    }

    const auto now = clock_();
    if (now < 0
        || now > std::numeric_limits<std::int64_t>::max() - intent_ttl_ms) {
        out.detail = "dialogue responder prepare clock/ttl is invalid";
        return out;
    }

    auto lease = responder_store_.find_lease(lease_id);
    if (!lease) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "dialogue responder lease not found";
        return out;
    }
    if (lease->state == LocalGooseDialogueResponderLeaseState::active
        && now >= lease->expires_at_ms) {
        const auto expired = responder_store_.close_lease(
            lease->lease_id,
            LocalGooseDialogueResponderLeaseState::expired,
            "lease_expired", now);
        if (expired.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && expired.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            out.result = map_store_result(expired.result);
            out.detail = "responder lease expiry normalization failed: "
                + expired.detail;
            return out;
        }
        lease = expired.lease;
    }
    if (!lease
        || lease->state != LocalGooseDialogueResponderLeaseState::active
        || now < lease->issued_at_ms
        || now >= lease->expires_at_ms
        || now < lease->next_eligible_at_ms
        || lease->turns_committed >= lease->max_system_turns) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "dialogue responder lease is not currently eligible";
        return out;
    }

    const auto event = completion_store_.next_for_consumer(consumer_id_);
    if (!event || event->sequence != completion_sequence) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "requested completion is not the responder consumer next event";
        return out;
    }
    if (event->speaker_kind != "human") {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "only a current human completion can authorize intervention";
        return out;
    }
    const auto head = thread_store_.find(lease->thread_alias);
    if (!head) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "responder preferred thread not found";
        return out;
    }
    out.head = head;
    if (event->thread_alias != lease->thread_alias
        || event->thread_revision != head->revision
        || event->task_id != head->head_task_id) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail =
            "completion event is not the current preferred responder head";
        return out;
    }

    const auto requested_expiry = now + intent_ttl_ms;
    const auto expiry = std::min(requested_expiry, lease->expires_at_ms);
    if (expiry <= now) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "responder intent would already be expired";
        return out;
    }

    const auto candidate = make_local_goose_dialogue_responder_intent(
        lease->lease_id,
        lease->thread_alias,
        event->sequence,
        event->event_id,
        event->result_sha256,
        event->task_id,
        head->revision,
        head->head_task_id,
        lease->speaker_id,
        message_kind,
        message,
        now,
        expiry);

    const auto intent_write = responder_store_.prepare_intent(candidate);
    out.intent = intent_write.intent;
    if ((intent_write.result
            != LocalGooseDialogueResponderStoreResult::accepted
         && intent_write.result
            != LocalGooseDialogueResponderStoreResult::duplicate)
        || !intent_write.intent) {
        out.result = map_store_result(intent_write.result);
        out.detail = intent_write.detail;
        attach_cursor(out, completion_store_, consumer_id_);
        return out;
    }
    if (intent_write.intent->state
        != LocalGooseDialogueResponderIntentState::prepared) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "responder intent is no longer in prepared state";
        attach_cursor(out, completion_store_, consumer_id_);
        return out;
    }

    const auto disposition_candidate =
        make_local_goose_dialogue_responder_disposition(
            consumer_id_, event->thread_alias, event->sequence,
            event->event_id, event->result_sha256,
            event->thread_revision, event->task_id,
            event->speaker_kind, event->speaker_id,
            LocalGooseDialogueResponderDispositionKind::intervention,
            intervention_authority_source,
            intent_write.intent->created_at_ms);

    bool changed =
        intent_write.result == LocalGooseDialogueResponderStoreResult::accepted;
    auto disposition = responder_store_.find_disposition_for_event(
        consumer_id_, event->event_id);
    if (!disposition) {
        const auto prepared =
            responder_store_.prepare_disposition(disposition_candidate);
        if ((prepared.result
                != LocalGooseDialogueResponderStoreResult::accepted
             && prepared.result
                != LocalGooseDialogueResponderStoreResult::duplicate)
            || !prepared.disposition) {
            out.result = map_store_result(prepared.result);
            out.detail = "intervention disposition prepare failed: "
                + prepared.detail;
            attach_cursor(out, completion_store_, consumer_id_);
            return out;
        }
        changed = changed
            || prepared.result == LocalGooseDialogueResponderStoreResult::accepted;
        disposition = prepared.disposition;
    }

    if (!disposition
        || !same_intervention_disposition_identity(
            *disposition, disposition_candidate)) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "completion event has a conflicting responder disposition";
        attach_cursor(out, completion_store_, consumer_id_);
        return out;
    }

    using DispositionState = LocalGooseDialogueResponderDispositionState;
    if (disposition->state == DispositionState::prepared) {
        const auto bound = responder_store_.bind_disposition_intent(
            disposition->disposition_id,
            intent_write.intent->intent_id,
            now);
        if ((bound.result
                != LocalGooseDialogueResponderStoreResult::accepted
             && bound.result
                != LocalGooseDialogueResponderStoreResult::duplicate)
            || !bound.disposition) {
            out.result = map_store_result(bound.result);
            out.detail = "intervention disposition binding failed: "
                + bound.detail;
            attach_cursor(out, completion_store_, consumer_id_);
            return out;
        }
        changed = changed
            || bound.result == LocalGooseDialogueResponderStoreResult::accepted;
        disposition = bound.disposition;
    }

    if (!disposition
        || disposition->state != DispositionState::intervention_bound
        || !bound_intervention_matches_intent(
            *disposition, *intent_write.intent, consumer_id_)) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "intervention disposition is not bound to the exact intent";
        attach_cursor(out, completion_store_, consumer_id_);
        return out;
    }

    out.result = changed
        ? LocalGooseDialogueResponderDispatchCode::accepted
        : LocalGooseDialogueResponderDispatchCode::duplicate;
    out.detail = changed
        ? "dialogue responder intent and intervention disposition prepared"
        : "dialogue responder intent and intervention disposition already prepared";
    attach_cursor(out, completion_store_, consumer_id_);
    return out;
}

LocalGooseDialogueResponderDispatchResult
LocalGooseDialogueResponderDispatcher::dispatch(
    const std::string& intent_id)
{
    LocalGooseDialogueResponderDispatchResult out;
    attach_cursor(out, completion_store_, consumer_id_);

    const auto stored_intent = responder_store_.find_intent(intent_id);
    if (!stored_intent) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "dialogue responder intent not found";
        return out;
    }
    out.intent = stored_intent;

    using IntentState = LocalGooseDialogueResponderIntentState;
    using DispositionState = LocalGooseDialogueResponderDispositionState;
    auto disposition = responder_store_.find_disposition_for_event(
        consumer_id_, stored_intent->event_id);

    if (stored_intent->state == IntentState::completed) {
        if (!disposition) {
            out.result = LocalGooseDialogueResponderDispatchCode::duplicate;
            out.detail =
                "legacy completed responder intent has no historical disposition";
            return out;
        }
        if (!bound_intervention_matches_intent(
                *disposition, *stored_intent, consumer_id_)) {
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.detail = "completed responder intent disposition conflicts";
            return out;
        }
        if (disposition->state == DispositionState::completed) {
            out.result = LocalGooseDialogueResponderDispatchCode::duplicate;
            out.detail = "dialogue responder intervention already completed";
            return out;
        }
        const auto now = clock_();
        if (now < 0) {
            out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
            out.detail = "dialogue responder clock is invalid";
            return out;
        }
        const auto completed_disposition =
            responder_store_.complete_disposition(
                disposition->disposition_id, now);
        if (completed_disposition.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && completed_disposition.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            out.result = map_store_result(completed_disposition.result);
            out.detail = "completed responder disposition reconciliation failed: "
                + completed_disposition.detail;
            return out;
        }
        out.result = LocalGooseDialogueResponderDispatchCode::duplicate;
        out.detail = "dialogue responder intervention completion reconciled";
        return out;
    }
    if (terminal_intent_state(stored_intent->state)) {
        const auto now = clock_();
        if (now < 0) {
            out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
            out.detail = "dialogue responder clock is invalid";
            return out;
        }
        const auto reconciled = reconcile_terminal_intervention_disposition(
            responder_store_, consumer_id_, *stored_intent, now);
        if (reconciled.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && reconciled.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            out.result = map_store_result(reconciled.result);
            out.detail = "terminal responder disposition reconciliation failed: "
                + reconciled.detail;
            return out;
        }
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = reconciled.result
                == LocalGooseDialogueResponderStoreResult::accepted
            ? "terminal responder disposition reconciled"
            : "dialogue responder intent and disposition are terminal";
        return out;
    }

    if (!disposition
        || disposition->state != DispositionState::intervention_bound
        || !bound_intervention_matches_intent(
            *disposition, *stored_intent, consumer_id_)) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail =
            "responder dispatch requires an exact bound intervention disposition";
        return out;
    }

    auto intent = *stored_intent;
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
        out.detail = "dialogue responder clock is invalid";
        return out;
    }

    const auto terminalize = [this, now, &out](
        const LocalGooseDialogueResponderIntent& source,
        const IntentState state,
        const std::string& reason) {
        const auto terminal = terminalize_intervention(
            responder_store_, consumer_id_, source, state, reason, now);
        if (terminal.intent) out.intent = terminal.intent;
        if (terminal.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && terminal.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            out.result = map_store_result(terminal.result);
            out.detail = "terminal responder intervention reconciliation failed: "
                + terminal.detail;
            return false;
        }
        return true;
    };

    if (intent.state == IntentState::prepared) {
        auto lease = responder_store_.find_lease(intent.lease_id);
        if (lease
            && lease->state == LocalGooseDialogueResponderLeaseState::active
            && now >= lease->expires_at_ms) {
            const auto expired = responder_store_.close_lease(
                lease->lease_id,
                LocalGooseDialogueResponderLeaseState::expired,
                "lease_expired", now);
            if (expired.result
                    != LocalGooseDialogueResponderStoreResult::accepted
                && expired.result
                    != LocalGooseDialogueResponderStoreResult::duplicate) {
                out.result = map_store_result(expired.result);
                out.detail =
                    "responder lease expiry normalization failed: "
                    + expired.detail;
                return out;
            }
            lease = expired.lease;
        }

        const auto event = completion_store_.next_for_consumer(consumer_id_);
        const auto head = thread_store_.find(intent.thread_alias);
        out.head = head;
        const bool trigger_still_next =
            event && same_trigger(*event, intent);
        const bool original_head =
            head
            && head->revision == intent.expected_thread_revision
            && head->head_task_id == intent.expected_head_task_id;
        const bool possible_idempotent_retry =
            head
            && intent.expected_thread_revision
                < static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            && head->revision == intent.expected_thread_revision + 1;

        const bool lease_eligible =
            lease
            && lease->state == LocalGooseDialogueResponderLeaseState::active
            && lease->thread_alias == intent.thread_alias
            && lease->speaker_kind == intent.speaker_kind
            && lease->speaker_id == intent.speaker_id
            && now < lease->expires_at_ms
            && now < intent.expires_at_ms
            && now >= lease->next_eligible_at_ms
            && lease->turns_committed < lease->max_system_turns;

        if (!lease_eligible) {
            if (trigger_still_next && possible_idempotent_retry) {
                const auto observed = submit_local_goose_dialogue_v3_preferred(
                    runtime_, task_store_, thread_store_, model_sha256_,
                    intent.request_id, intent.thread_alias,
                    intent.expected_thread_revision, intent.speaker_kind,
                    intent.speaker_id, intent.message_kind, intent.message);
                out.task = observed.task;
                out.head = observed.head;
                out.work_may_be_pending = observed.work_may_be_pending;
                if (observed.result
                        == LocalGooseDialoguePreferredSubmitResultCode::duplicate
                    || observed.task) {
                    if (!terminalize(
                            intent, IntentState::manual_review,
                            "durable_submission_with_ineligible_lease")) {
                        return out;
                    }
                    out.result =
                        LocalGooseDialogueResponderDispatchCode::conflict;
                    out.detail =
                        "durable responder submission exists but lease authority "
                        "cannot be proven current; manual review required";
                    return out;
                }
            }

            const bool expired =
                now >= intent.expires_at_ms
                || (lease
                    && lease->state
                        == LocalGooseDialogueResponderLeaseState::expired);
            if (!terminalize(
                    intent,
                    expired ? IntentState::expired : IntentState::conflict,
                    expired
                        ? "lease_or_intent_expired"
                        : "lease_not_eligible")) {
                return out;
            }
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.detail = expired
                ? "responder intent or lease expired"
                : "responder lease is no longer eligible";
            return out;
        }

        if (!trigger_still_next
            || (!original_head && !possible_idempotent_retry)) {
            if (!terminalize(
                    intent, IntentState::conflict,
                    "trigger_or_preferred_head_changed")) {
                return out;
            }
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.detail = "responder trigger/head evidence is stale";
            return out;
        }

        const auto submit = submit_local_goose_dialogue_v3_preferred(
            runtime_, task_store_, thread_store_, model_sha256_,
            intent.request_id, intent.thread_alias,
            intent.expected_thread_revision, intent.speaker_kind,
            intent.speaker_id, intent.message_kind, intent.message);
        out.task = submit.task;
        out.head = submit.head;
        out.work_may_be_pending = submit.work_may_be_pending;

        if (submit.result
                != LocalGooseDialoguePreferredSubmitResultCode::accepted
            && submit.result
                != LocalGooseDialoguePreferredSubmitResultCode::duplicate) {
            const auto terminal_state =
                (submit.task
                 || submit.result
                    == LocalGooseDialoguePreferredSubmitResultCode::unavailable)
                ? IntentState::manual_review
                : IntentState::conflict;
            if (!terminalize(
                    intent, terminal_state,
                    terminal_state == IntentState::manual_review
                        ? "preferred_submit_ambiguous"
                        : "preferred_submit_rejected")) {
                return out;
            }
            out.result = map_submit_result(submit.result);
            out.detail = submit.detail;
            return out;
        }

        if (!submit.task || !submit.head
            || submit.head->revision != intent.expected_thread_revision + 1
            || submit.head->head_task_id != submit.task->id) {
            if (!terminalize(
                    intent, IntentState::manual_review,
                    "preferred_submit_result_incoherent")) {
                return out;
            }
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.detail = "preferred responder submission result is incoherent";
            return out;
        }

        const auto committed = responder_store_.commit_submission(
            intent.intent_id, submit.task->id, submit.head->revision, now);
        if (committed.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && committed.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            if (!terminalize(
                    intent, IntentState::manual_review,
                    "submission_accounting_conflict")) {
                return out;
            }
            out.result = map_store_result(committed.result);
            out.detail = "responder submission accounting failed: "
                + committed.detail;
            return out;
        }
        if (!committed.intent) {
            out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
            out.detail = "reconciled responder intent is missing";
            return out;
        }
        intent = *committed.intent;
        out.intent = intent;
    }

    if (intent.state != IntentState::submitted
        || !intent.submitted_thread_revision
        || intent.submitted_task_id.empty()
        || !intent.committed_at_ms) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "responder intent is not dispatch-recoverable";
        return out;
    }

    disposition = responder_store_.find_disposition_for_event(
        consumer_id_, intent.event_id);
    if (!disposition
        || disposition->state != DispositionState::intervention_bound
        || !bound_intervention_matches_intent(
            *disposition, intent, consumer_id_)) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail =
            "bound intervention disposition was lost before trigger ACK";
        return out;
    }

    const auto task = task_store_.find(intent.submitted_task_id);
    const auto head = thread_store_.find(intent.thread_alias);
    out.task = task;
    out.head = head;
    if (!task || !same_submitted_task(*task, intent)
        || !head
        || head->revision != *intent.submitted_thread_revision
        || head->head_task_id != intent.submitted_task_id) {
        if (!terminalize(
                intent, IntentState::manual_review,
                "submitted_task_or_head_mismatch")) {
            return out;
        }
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "submitted responder Task/head cannot be reconciled";
        return out;
    }
    out.work_may_be_pending = !gaudere::work::is_terminal(task->status);

    const auto cursor = completion_store_.consumer_last_sequence(consumer_id_);
    if (!cursor) {
        out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
        out.detail = "responder consumer cursor is unavailable";
        return out;
    }
    out.consumer_last_sequence = *cursor;

    if (*cursor > intent.completion_sequence) {
        if (!terminalize(
                intent, IntentState::manual_review,
                "consumer_cursor_advanced_past_trigger")) {
            return out;
        }
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "responder consumer cursor advanced past trigger";
        return out;
    }

    if (*cursor < intent.completion_sequence) {
        const auto event = completion_store_.next_for_consumer(consumer_id_);
        if (!event || !same_trigger(*event, intent)) {
            if (!terminalize(
                    intent, IntentState::manual_review,
                    "trigger_not_next_before_ack")) {
                return out;
            }
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.detail = "responder trigger is no longer next before ACK";
            return out;
        }
    }

    const auto ack = completion_store_.acknowledge(
        consumer_id_, intent.completion_sequence);
    if (ack.result != LocalGooseDialogueCompletionFeedResult::accepted
        && ack.result != LocalGooseDialogueCompletionFeedResult::duplicate) {
        if (ack.result == LocalGooseDialogueCompletionFeedResult::unavailable) {
            out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
            out.detail = "responder trigger ACK unavailable: " + ack.detail;
            return out;
        }
        if (!terminalize(
                intent, IntentState::manual_review,
                "trigger_ack_conflict")) {
            return out;
        }
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.consumer_last_sequence = ack.last_sequence;
        out.detail = "responder trigger ACK failed: " + ack.detail;
        return out;
    }
    out.consumer_last_sequence = ack.last_sequence;

    const auto completed =
        responder_store_.complete_intent(intent.intent_id, now);
    if ((completed.result
            != LocalGooseDialogueResponderStoreResult::accepted
         && completed.result
            != LocalGooseDialogueResponderStoreResult::duplicate)
        || !completed.intent) {
        out.result = map_store_result(completed.result);
        out.intent = completed.intent;
        out.detail = "responder intent completion failed: " + completed.detail;
        return out;
    }
    out.intent = completed.intent;

    const auto completed_disposition = responder_store_.complete_disposition(
        disposition->disposition_id, now);
    if (completed_disposition.result
            != LocalGooseDialogueResponderStoreResult::accepted
        && completed_disposition.result
            != LocalGooseDialogueResponderStoreResult::duplicate) {
        out.result = map_store_result(completed_disposition.result);
        out.detail = "responder intervention disposition completion failed: "
            + completed_disposition.detail;
        return out;
    }

    out.result = completed.result
        == LocalGooseDialogueResponderStoreResult::duplicate
        ? LocalGooseDialogueResponderDispatchCode::duplicate
        : LocalGooseDialogueResponderDispatchCode::accepted;
    out.detail = "dialogue responder intervention completed";
    return out;
}

} // namespace gaudere_agent
