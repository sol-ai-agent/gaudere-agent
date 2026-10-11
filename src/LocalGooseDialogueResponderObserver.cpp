#include "LocalGooseDialogueResponderObserver.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Classification = LocalGooseDialogueResponderObserverClassification;
using Action = LocalGooseDialogueResponderObserverAction;
using Disposition = LocalGooseDialogueResponderDisposition;
using DispositionKind = LocalGooseDialogueResponderDispositionKind;
using DispositionState = LocalGooseDialogueResponderDispositionState;
using StoreResult = LocalGooseDialogueResponderStoreResult;
using FeedResult = LocalGooseDialogueCompletionFeedResult;

bool exact_event_matches_disposition(
    const LocalGooseDialogueCompletionEvent& event,
    const Disposition& disposition) noexcept
{
    return event.sequence == disposition.completion_sequence
        && event.event_id == disposition.event_id
        && event.thread_alias == disposition.thread_alias
        && event.thread_revision == disposition.thread_revision
        && event.task_id == disposition.task_id
        && event.speaker_kind == disposition.speaker_kind
        && event.speaker_id == disposition.speaker_id
        && event.result_sha256 == disposition.result_sha256;
}

bool exact_event_matches_authorization(
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueResponderExplicitDecline& authorization) noexcept
{
    return event.sequence == authorization.completion_sequence
        && event.event_id == authorization.event_id
        && event.result_sha256 == authorization.result_sha256
        && event.thread_revision == authorization.thread_revision
        && event.task_id == authorization.task_id;
}

bool exact_head_matches_authorization(
    const LocalGooseDialogueThreadHead& head,
    const LocalGooseDialogueResponderExplicitDecline& authorization) noexcept
{
    return head.revision == authorization.expected_head_revision
        && head.head_task_id == authorization.expected_head_task_id;
}

bool non_intervention_kind(const DispositionKind kind) noexcept
{
    return kind == DispositionKind::structural_skip_system
        || kind == DispositionKind::structural_skip_stale
        || kind == DispositionKind::explicit_decline;
}

Classification classify_event(
    const LocalGooseDialogueCompletionEvent& event,
    const std::optional<LocalGooseDialogueThreadHead>& head,
    const std::optional<Disposition>& disposition,
    const std::string& expected_alias) noexcept
{
    if (!valid_local_goose_dialogue_completion_event(event)
        || event.thread_alias != expected_alias
        || !head
        || head->alias != expected_alias
        || event.root_task_id != head->root_task_id
        || event.thread_revision > head->revision
        || (event.thread_revision == head->revision
            && event.task_id != head->head_task_id)) {
        return Classification::ambiguous;
    }

    if (disposition) {
        if (!exact_event_matches_disposition(event, *disposition)) {
            return Classification::ambiguous;
        }
        if (disposition->kind == DispositionKind::intervention) {
            return Classification::intervention_owned;
        }
        if (disposition->state == DispositionState::prepared) {
            return Classification::reconciliation_pending;
        }
        // A non-intervention disposition whose durable state says the event was
        // already reconciled must not still be the consumer's next event.
        return Classification::ambiguous;
    }

    if (event.speaker_kind == "system") {
        return Classification::structural_system;
    }
    if (event.speaker_kind != "human") {
        return Classification::ambiguous;
    }
    if (event.thread_revision < head->revision) {
        return Classification::structural_stale;
    }
    return Classification::current_human;
}

std::optional<Disposition> earliest_prepared_for_consumer(
    LocalGooseDialogueResponderStore& store,
    const std::string& consumer_id)
{
    auto prepared = store.dispositions_in_state(DispositionState::prepared, 1024);
    prepared.erase(
        std::remove_if(
            prepared.begin(), prepared.end(),
            [&](const Disposition& disposition) {
                return disposition.consumer_id != consumer_id;
            }),
        prepared.end());
    if (prepared.empty()) return std::nullopt;
    std::sort(
        prepared.begin(), prepared.end(),
        [](const Disposition& left, const Disposition& right) {
            if (left.completion_sequence != right.completion_sequence) {
                return left.completion_sequence < right.completion_sequence;
            }
            return left.disposition_id < right.disposition_id;
        });
    return prepared.front();
}

LocalGooseDialogueResponderObserverStepResult unavailable_result(
    LocalGooseDialogueResponderObserverStatus status,
    const std::string& detail)
{
    LocalGooseDialogueResponderObserverStepResult out;
    out.action = Action::unavailable;
    out.status = std::move(status);
    out.detail = detail;
    return out;
}

} // namespace

LocalGooseDialogueResponderObserver::LocalGooseDialogueResponderObserver(
    LocalGooseDialogueCompletionFeedStore& feed_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderObserverConfig config,
    LocalGooseDialogueResponderObserverClock clock)
    : feed_store_(feed_store),
      thread_store_(thread_store),
      responder_store_(responder_store),
      config_(std::move(config)),
      clock_(std::move(clock))
{
    if (config_.consumer_id.empty()
        || config_.thread_alias.empty()
        || config_.structural_authority_source.empty()
        || !clock_) {
        throw std::invalid_argument("invalid dialogue responder observer config");
    }
}

LocalGooseDialogueResponderObserverStatus
LocalGooseDialogueResponderObserver::inspect_current() const
{
    LocalGooseDialogueResponderObserverStatus status;
    status.observation_armed = config_.observation_armed;
    status.consumer_id = config_.consumer_id;
    status.thread_alias = config_.thread_alias;

    try {
        status.consumer_last_sequence =
            feed_store_.consumer_last_sequence(config_.consumer_id);
        status.next_event = feed_store_.next_for_consumer(config_.consumer_id);
        status.preferred_head = thread_store_.find(config_.thread_alias);
        if (!status.next_event) {
            status.classification = Classification::no_event;
            status.detail = "no pending dialogue completion event";
            return status;
        }

        status.disposition = responder_store_.find_disposition_for_event(
            config_.consumer_id, status.next_event->event_id);
        status.classification = classify_event(
            *status.next_event,
            status.preferred_head,
            status.disposition,
            config_.thread_alias);

        switch (status.classification) {
        case Classification::structural_system:
            status.detail = "next completion is system-authored and non-intervenable";
            break;
        case Classification::structural_stale:
            status.detail = "next completion is stale relative to preferred head";
            break;
        case Classification::current_human:
            status.detail = "current human preferred-head completion is held";
            break;
        case Classification::intervention_owned:
            status.detail = "completion is owned by intervention disposition";
            break;
        case Classification::reconciliation_pending:
            status.detail = "non-intervention disposition awaits ACK reconciliation";
            break;
        case Classification::ambiguous:
            status.detail = "completion/head/disposition evidence is ambiguous";
            break;
        case Classification::no_event:
            status.detail = "no pending dialogue completion event";
            break;
        }
        return status;
    } catch (const std::exception& error) {
        status.classification = Classification::ambiguous;
        status.detail = error.what();
        return status;
    }
}

LocalGooseDialogueResponderObserverStatus
LocalGooseDialogueResponderObserver::inspect() const
{
    return inspect_current();
}

LocalGooseDialogueResponderObserverStepResult
LocalGooseDialogueResponderObserver::step()
{
    auto status = inspect_current();
    LocalGooseDialogueResponderObserverStepResult out;
    out.status = status;

    if (!config_.observation_armed) {
        out.action = Action::disarmed;
        out.detail = "observer mode is disabled; no state was changed";
        return out;
    }

    try {
        const auto now = clock_();
        if (now < 0) {
            return unavailable_result(
                std::move(status), "observer clock returned a negative time");
        }

        // Recover the oldest durable non-intervention decision before making a
        // fresh one. This is what closes the crash window after feed ACK but
        // before responder-sidecar reconciliation.
        const auto prepared = earliest_prepared_for_consumer(
            responder_store_, config_.consumer_id);
        if (prepared) {
            out.status.disposition = prepared;
            if (prepared->kind == DispositionKind::intervention) {
                out.status.classification = Classification::intervention_owned;
                out.action = Action::held;
                out.detail =
                    "prepared intervention belongs to responder dispatch path";
                return out;
            }
            if (!non_intervention_kind(prepared->kind)) {
                const auto terminal = responder_store_.terminalize_disposition(
                    prepared->disposition_id,
                    "observer_unknown_prepared_disposition_kind",
                    now);
                out.action = terminal.result == StoreResult::unavailable
                    ? Action::unavailable : Action::manual_review;
                out.detail = terminal.detail;
                out.status = inspect_current();
                return out;
            }

            const auto cursor = feed_store_.consumer_last_sequence(
                config_.consumer_id).value_or(0);
            if (cursor == prepared->completion_sequence) {
                const auto reconciled = responder_store_.acknowledge_disposition(
                    prepared->disposition_id, now);
                if (reconciled.result == StoreResult::accepted
                    || reconciled.result == StoreResult::duplicate) {
                    out.action = Action::reconciled;
                    out.detail =
                        "reconciled disposition after previously durable feed ACK";
                } else {
                    out.action = Action::unavailable;
                    out.detail = reconciled.detail;
                }
                out.status = inspect_current();
                return out;
            }

            if (cursor > prepared->completion_sequence
                || cursor == std::numeric_limits<std::uint64_t>::max()
                || cursor + 1 != prepared->completion_sequence) {
                const auto terminal = responder_store_.terminalize_disposition(
                    prepared->disposition_id,
                    "observer_cursor_gap_for_prepared_disposition",
                    now);
                out.action = terminal.result == StoreResult::unavailable
                    ? Action::unavailable : Action::manual_review;
                out.detail = terminal.detail;
                out.status = inspect_current();
                return out;
            }

            const auto next = feed_store_.next_for_consumer(config_.consumer_id);
            if (!next || !exact_event_matches_disposition(*next, *prepared)) {
                const auto terminal = responder_store_.terminalize_disposition(
                    prepared->disposition_id,
                    "observer_next_event_mismatch_for_prepared_disposition",
                    now);
                out.action = terminal.result == StoreResult::unavailable
                    ? Action::unavailable : Action::manual_review;
                out.detail = terminal.detail;
                out.status = inspect_current();
                return out;
            }

            const auto ack = feed_store_.acknowledge(
                config_.consumer_id, prepared->completion_sequence);
            if (!(ack.result == FeedResult::accepted
                  || ack.result == FeedResult::duplicate)) {
                out.action = Action::unavailable;
                out.detail = ack.detail;
                out.status = inspect_current();
                return out;
            }
            const auto reconciled = responder_store_.acknowledge_disposition(
                prepared->disposition_id, now);
            if (!(reconciled.result == StoreResult::accepted
                  || reconciled.result == StoreResult::duplicate)) {
                // The feed ACK is already durable. Leave the disposition
                // prepared so the next step can reconcile it from the cursor.
                out.action = Action::unavailable;
                out.detail = reconciled.detail;
                out.status = inspect_current();
                return out;
            }
            out.action = Action::acknowledged;
            out.detail = "prepared non-intervention disposition ACKed exactly";
            out.status = inspect_current();
            return out;
        }

        // No durable decision is awaiting recovery. Fresh automatic authority
        // is intentionally restricted to structural non-intervention events.
        status = inspect_current();
        out.status = status;
        if (!status.next_event) {
            out.action = Action::none;
            out.detail = "no pending completion event";
            return out;
        }
        if (status.classification == Classification::current_human
            || status.classification == Classification::intervention_owned
            || status.classification == Classification::reconciliation_pending
            || status.classification == Classification::ambiguous) {
            out.action = Action::held;
            out.detail = status.detail;
            return out;
        }
        if (!(status.classification == Classification::structural_system
              || status.classification == Classification::structural_stale)) {
            out.action = Action::none;
            out.detail = status.detail;
            return out;
        }

        const auto kind = status.classification == Classification::structural_system
            ? DispositionKind::structural_skip_system
            : DispositionKind::structural_skip_stale;
        const auto& event = *status.next_event;
        auto disposition = make_local_goose_dialogue_responder_disposition(
            config_.consumer_id,
            event.thread_alias,
            event.sequence,
            event.event_id,
            event.result_sha256,
            event.thread_revision,
            event.task_id,
            event.speaker_kind,
            event.speaker_id,
            kind,
            config_.structural_authority_source,
            now);
        const auto prepared_write = responder_store_.prepare_disposition(disposition);
        if (!(prepared_write.result == StoreResult::accepted
              || prepared_write.result == StoreResult::duplicate)
            || !prepared_write.disposition) {
            out.action = prepared_write.result == StoreResult::unavailable
                ? Action::unavailable : Action::held;
            out.detail = prepared_write.detail;
            out.status = inspect_current();
            return out;
        }

        // Re-read and reclassify after the durable cause exists. Never ACK an
        // event that changed identity or ceased to satisfy the same structural
        // rule in the meantime.
        const auto next = feed_store_.next_for_consumer(config_.consumer_id);
        const auto head = thread_store_.find(config_.thread_alias);
        const auto existing = responder_store_.find_disposition_for_event(
            config_.consumer_id, event.event_id);
        const auto reclassification = next
            ? classify_event(*next, head, existing, config_.thread_alias)
            : Classification::ambiguous;
        const bool exact = next
            && exact_event_matches_disposition(*next, *prepared_write.disposition);
        const bool same_rule =
            (kind == DispositionKind::structural_skip_system
             && (reclassification == Classification::reconciliation_pending
                 || reclassification == Classification::structural_system))
            || (kind == DispositionKind::structural_skip_stale
                && (reclassification == Classification::reconciliation_pending
                    || reclassification == Classification::structural_stale));
        if (!exact || !same_rule) {
            const auto terminal = responder_store_.terminalize_disposition(
                prepared_write.disposition->disposition_id,
                "observer_structural_revalidation_failed",
                now);
            out.action = terminal.result == StoreResult::unavailable
                ? Action::unavailable : Action::manual_review;
            out.detail = terminal.detail;
            out.status = inspect_current();
            return out;
        }

        const auto ack = feed_store_.acknowledge(
            config_.consumer_id, event.sequence);
        if (!(ack.result == FeedResult::accepted
              || ack.result == FeedResult::duplicate)) {
            out.action = Action::unavailable;
            out.detail = ack.detail;
            out.status = inspect_current();
            return out;
        }
        const auto reconciled = responder_store_.acknowledge_disposition(
            prepared_write.disposition->disposition_id, now);
        if (!(reconciled.result == StoreResult::accepted
              || reconciled.result == StoreResult::duplicate)) {
            out.action = Action::unavailable;
            out.detail = reconciled.detail;
            out.status = inspect_current();
            return out;
        }
        out.action = Action::acknowledged;
        out.detail = "structural completion retired with durable disposition";
        out.status = inspect_current();
        return out;
    } catch (const std::exception& error) {
        return unavailable_result(std::move(status), error.what());
    }
}

LocalGooseDialogueResponderObserverStepResult
LocalGooseDialogueResponderObserver::decline(
    const LocalGooseDialogueResponderExplicitDecline& authorization)
{
    auto status = inspect_current();
    LocalGooseDialogueResponderObserverStepResult out;
    out.status = status;

    if (!config_.observation_armed) {
        out.action = Action::disarmed;
        out.detail = "observer mode is disabled; explicit decline was not applied";
        return out;
    }
    if (authorization.authority_source.empty()
        || authorization.completion_sequence == 0
        || authorization.event_id.empty()
        || authorization.result_sha256.empty()
        || authorization.task_id.empty()
        || authorization.expected_head_task_id.empty()) {
        out.action = Action::held;
        out.detail = "explicit decline authorization is incomplete";
        return out;
    }

    try {
        const auto now = clock_();
        if (now < 0) {
            return unavailable_result(
                std::move(status), "observer clock returned a negative time");
        }
        if (!status.next_event || !status.preferred_head
            || status.classification != Classification::current_human
            || !exact_event_matches_authorization(
                *status.next_event, authorization)
            || !exact_head_matches_authorization(
                *status.preferred_head, authorization)) {
            out.action = Action::held;
            out.detail =
                "explicit decline does not match exact current human preferred head";
            return out;
        }

        const auto& event = *status.next_event;
        auto disposition = make_local_goose_dialogue_responder_disposition(
            config_.consumer_id,
            event.thread_alias,
            event.sequence,
            event.event_id,
            event.result_sha256,
            event.thread_revision,
            event.task_id,
            event.speaker_kind,
            event.speaker_id,
            DispositionKind::explicit_decline,
            authorization.authority_source,
            now);
        const auto prepared = responder_store_.prepare_disposition(disposition);
        if (!(prepared.result == StoreResult::accepted
              || prepared.result == StoreResult::duplicate)
            || !prepared.disposition) {
            out.action = prepared.result == StoreResult::unavailable
                ? Action::unavailable : Action::held;
            out.detail = prepared.detail;
            out.status = inspect_current();
            return out;
        }

        const auto next = feed_store_.next_for_consumer(config_.consumer_id);
        const auto head = thread_store_.find(config_.thread_alias);
        if (!next || !head
            || !exact_event_matches_disposition(*next, *prepared.disposition)
            || !exact_event_matches_authorization(*next, authorization)
            || !exact_head_matches_authorization(*head, authorization)
            || next->speaker_kind != "human"
            || next->thread_revision != head->revision
            || next->task_id != head->head_task_id) {
            const auto terminal = responder_store_.terminalize_disposition(
                prepared.disposition->disposition_id,
                "explicit_decline_revalidation_failed",
                now);
            out.action = terminal.result == StoreResult::unavailable
                ? Action::unavailable : Action::manual_review;
            out.detail = terminal.detail;
            out.status = inspect_current();
            return out;
        }

        const auto ack = feed_store_.acknowledge(
            config_.consumer_id, event.sequence);
        if (!(ack.result == FeedResult::accepted
              || ack.result == FeedResult::duplicate)) {
            out.action = Action::unavailable;
            out.detail = ack.detail;
            out.status = inspect_current();
            return out;
        }
        const auto reconciled = responder_store_.acknowledge_disposition(
            prepared.disposition->disposition_id, now);
        if (!(reconciled.result == StoreResult::accepted
              || reconciled.result == StoreResult::duplicate)) {
            out.action = Action::unavailable;
            out.detail = reconciled.detail;
            out.status = inspect_current();
            return out;
        }

        out.action = Action::acknowledged;
        out.detail = "explicit decline durably reconciled and exact event ACKed";
        out.status = inspect_current();
        return out;
    } catch (const std::exception& error) {
        return unavailable_result(std::move(status), error.what());
    }
}

} // namespace gaudere_agent
