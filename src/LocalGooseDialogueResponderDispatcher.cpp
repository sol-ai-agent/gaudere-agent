#include "LocalGooseDialogueResponderDispatcher.hpp"

#include "LocalGooseDialogueV3.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr std::int64_t max_intent_ttl_ms = 24LL * 60 * 60 * 1000;

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

    const auto lease = responder_store_.find_lease(lease_id);
    if (!lease) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "dialogue responder lease not found";
        return out;
    }
    if (lease->state != LocalGooseDialogueResponderLeaseState::active
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

    const auto write = responder_store_.prepare_intent(candidate);
    out.result = map_store_result(write.result);
    out.intent = write.intent;
    out.detail = write.detail;
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
    if (stored_intent->state == IntentState::completed) {
        out.result = LocalGooseDialogueResponderDispatchCode::duplicate;
        out.detail = "dialogue responder intent already completed";
        return out;
    }
    if (stored_intent->state == IntentState::conflict
        || stored_intent->state == IntentState::expired
        || stored_intent->state == IntentState::manual_review) {
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.detail = "dialogue responder intent is terminal";
        return out;
    }

    auto intent = *stored_intent;
    const auto now = clock_();
    if (now < 0) {
        out.result = LocalGooseDialogueResponderDispatchCode::unavailable;
        out.detail = "dialogue responder clock is invalid";
        return out;
    }

    if (intent.state == IntentState::prepared) {
        const auto lease = responder_store_.find_lease(intent.lease_id);
        if (!lease
            || lease->state != LocalGooseDialogueResponderLeaseState::active
            || lease->thread_alias != intent.thread_alias
            || lease->speaker_kind != intent.speaker_kind
            || lease->speaker_id != intent.speaker_id
            || now >= lease->expires_at_ms
            || now >= intent.expires_at_ms
            || now < lease->next_eligible_at_ms
            || lease->turns_committed >= lease->max_system_turns) {
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, IntentState::expired,
                "lease_or_intent_not_eligible");
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.intent = terminal.intent ? terminal.intent : out.intent;
            out.detail = "responder intent expired or lease is no longer eligible";
            return out;
        }

        const auto event = completion_store_.next_for_consumer(consumer_id_);
        const auto head = thread_store_.find(intent.thread_alias);
        out.head = head;
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
        if (!event || !same_trigger(*event, intent)
            || (!original_head && !possible_idempotent_retry)) {
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, IntentState::conflict,
                "trigger_or_preferred_head_changed");
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.intent = terminal.intent ? terminal.intent : out.intent;
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
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, terminal_state,
                terminal_state == IntentState::manual_review
                    ? "preferred_submit_ambiguous"
                    : "preferred_submit_rejected");
            out.result = map_submit_result(submit.result);
            out.intent = terminal.intent ? terminal.intent : out.intent;
            out.detail = submit.detail;
            return out;
        }

        if (!submit.task || !submit.head
            || submit.head->revision != intent.expected_thread_revision + 1
            || submit.head->head_task_id != submit.task->id) {
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, IntentState::manual_review,
                "preferred_submit_result_incoherent");
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.intent = terminal.intent ? terminal.intent : out.intent;
            out.detail = "preferred responder submission result is incoherent";
            return out;
        }

        const auto committed = responder_store_.commit_submission(
            intent.intent_id, submit.task->id, submit.head->revision, now);
        if (committed.result
                != LocalGooseDialogueResponderStoreResult::accepted
            && committed.result
                != LocalGooseDialogueResponderStoreResult::duplicate) {
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, IntentState::manual_review,
                "submission_accounting_conflict");
            out.result = map_store_result(committed.result);
            out.intent = terminal.intent ? terminal.intent : committed.intent;
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

    const auto task = task_store_.find(intent.submitted_task_id);
    const auto head = thread_store_.find(intent.thread_alias);
    out.task = task;
    out.head = head;
    if (!task || !same_submitted_task(*task, intent)
        || !head
        || head->revision != *intent.submitted_thread_revision
        || head->head_task_id != intent.submitted_task_id) {
        const auto terminal = responder_store_.terminalize_intent(
            intent.intent_id, IntentState::manual_review,
            "submitted_task_or_head_mismatch");
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.intent = terminal.intent ? terminal.intent : out.intent;
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
        const auto terminal = responder_store_.terminalize_intent(
            intent.intent_id, IntentState::manual_review,
            "consumer_cursor_advanced_past_trigger");
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.intent = terminal.intent ? terminal.intent : out.intent;
        out.detail = "responder consumer cursor advanced past trigger";
        return out;
    }

    if (*cursor < intent.completion_sequence) {
        const auto event = completion_store_.next_for_consumer(consumer_id_);
        if (!event || !same_trigger(*event, intent)) {
            const auto terminal = responder_store_.terminalize_intent(
                intent.intent_id, IntentState::manual_review,
                "trigger_not_next_before_ack");
            out.result = LocalGooseDialogueResponderDispatchCode::conflict;
            out.intent = terminal.intent ? terminal.intent : out.intent;
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
        const auto terminal = responder_store_.terminalize_intent(
            intent.intent_id, IntentState::manual_review,
            "trigger_ack_conflict");
        out.result = LocalGooseDialogueResponderDispatchCode::conflict;
        out.intent = terminal.intent ? terminal.intent : out.intent;
        out.consumer_last_sequence = ack.last_sequence;
        out.detail = "responder trigger ACK failed: " + ack.detail;
        return out;
    }
    out.consumer_last_sequence = ack.last_sequence;

    const auto completed =
        responder_store_.complete_intent(intent.intent_id, now);
    out.result = map_store_result(completed.result);
    out.intent = completed.intent;
    out.detail = completed.detail;
    if (completed.result == LocalGooseDialogueResponderStoreResult::duplicate) {
        out.result = LocalGooseDialogueResponderDispatchCode::duplicate;
    }
    return out;
}

} // namespace gaudere_agent
