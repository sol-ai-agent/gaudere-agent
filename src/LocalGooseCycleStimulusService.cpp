#include "LocalGooseCycleStimulusService.hpp"

#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using ServiceResult = LocalGooseCycleStimulusServiceResult;
using ServiceStep = LocalGooseCycleStimulusServiceStep;
using Stimulus = LocalGooseCycleStimulus;
using StimulusStatus = LocalGooseCycleStimulusStatus;
using StimulusStoreResult = LocalGooseCycleStimulusStoreResult;
using CycleCursor = LocalGooseCycleCursor;
using CycleState = LocalGooseCycleState;
using CycleStoreResult = LocalGooseCycleStoreResult;

ServiceResult terminal_result(const StimulusStatus status)
{
    switch (status) {
    case StimulusStatus::accepted:
        return ServiceResult::accepted;
    case StimulusStatus::consumed:
        return ServiceResult::consumed;
    case StimulusStatus::superseded:
        return ServiceResult::superseded;
    case StimulusStatus::manual_review:
        return ServiceResult::manual_review;
    }
    return ServiceResult::invalid;
}

bool canonical_predecessor(
    const CycleCursor& cursor,
    gaudere::work::TaskStore& task_store,
    std::string& detail)
{
    if (cursor.generation < 2
        || !cursor.predecessor_task_id
        || !cursor.predecessor_result_sha256) {
        detail = "dormant Local Goose cycle lacks predecessor evidence";
        return false;
    }

    const auto predecessor = task_store.find(*cursor.predecessor_task_id);
    if (!predecessor || !predecessor->result
        || !canonical_local_goose_cycle_success(*predecessor)) {
        detail =
            "Local Goose cycle predecessor Task/result is missing or non-canonical";
        return false;
    }
    if (sha256_hex(predecessor->result->output)
        != *cursor.predecessor_result_sha256) {
        detail = "Local Goose cycle predecessor result hash differs";
        return false;
    }

    const auto inspected = inspect_local_goose_cycle_task(*predecessor);
    if (!inspected.eligible
        || inspected.generation == std::numeric_limits<std::uint64_t>::max()
        || inspected.generation + 1 != cursor.generation
        || inspected.anchor_observation_task_id
            != cursor.anchor_observation_task_id
        || inspected.anchor_observation_result_sha256
            != cursor.anchor_observation_result_sha256) {
        detail = "Local Goose cycle predecessor lineage differs from cursor";
        return false;
    }
    return true;
}

bool exact_rearmed_cursor(
    const CycleCursor& cursor,
    const Stimulus& stimulus,
    gaudere::work::TaskStore& task_store,
    std::string& detail)
{
    if (stimulus.target_cycle_revision
        == std::numeric_limits<std::uint64_t>::max()) {
        detail = "stimulus target revision cannot have a successor";
        return false;
    }
    if (cursor.revision != stimulus.target_cycle_revision + 1
        || cursor.generation != stimulus.target_cycle_generation
        || cursor.state != CycleState::scheduled
        || cursor.due_at_ms
            != std::optional<std::int64_t>{stimulus.accepted_at_ms}
        || cursor.captured_at_ms
        || !cursor.current_task_id.empty()
        || !cursor.blocked_reason.empty()) {
        return false;
    }
    return canonical_predecessor(cursor, task_store, detail);
}

ServiceStep terminalize(
    LocalGooseCycleStimulusStore& stimulus_store,
    const Stimulus& accepted,
    const StimulusStatus status,
    const std::int64_t observed_at_ms,
    const std::optional<std::uint64_t> resulting_cycle_revision,
    std::string reason,
    const std::optional<CycleCursor>& cursor)
{
    Stimulus replacement = accepted;
    replacement.status = status;
    replacement.terminal_at_ms =
        std::max(observed_at_ms, accepted.accepted_at_ms);
    replacement.resulting_cycle_revision = resulting_cycle_revision;
    replacement.terminal_reason = std::move(reason);

    const auto write = stimulus_store.replace(accepted, replacement);
    if (write.result == StimulusStoreResult::accepted
        || write.result == StimulusStoreResult::duplicate) {
        const auto stored = write.stimulus
            ? write.stimulus
            : std::optional<Stimulus>{replacement};
        return {terminal_result(stored->status), stored, cursor, {}};
    }
    if (write.result == StimulusStoreResult::conflict && write.stimulus
        && write.stimulus->status != StimulusStatus::accepted) {
        return {terminal_result(write.stimulus->status),
                write.stimulus, cursor, write.detail};
    }
    return {
        write.result == StimulusStoreResult::unavailable
            ? ServiceResult::unavailable
            : ServiceResult::conflict,
        write.stimulus, cursor,
        write.detail.empty()
            ? "Local Goose cycle stimulus terminal transition failed"
            : write.detail};
}

ServiceStep manual_review(
    LocalGooseCycleStimulusStore& stimulus_store,
    const Stimulus& accepted,
    const std::int64_t observed_at_ms,
    const std::string& reason,
    const std::optional<CycleCursor>& cursor)
{
    return terminalize(
        stimulus_store, accepted, StimulusStatus::manual_review,
        observed_at_ms, std::nullopt, reason, cursor);
}

ServiceStep supersede(
    LocalGooseCycleStimulusStore& stimulus_store,
    const Stimulus& accepted,
    const std::int64_t observed_at_ms,
    const std::string& reason,
    const std::optional<CycleCursor>& cursor)
{
    return terminalize(
        stimulus_store, accepted, StimulusStatus::superseded,
        observed_at_ms, std::nullopt, reason, cursor);
}

} // namespace

LocalGooseCycleStimulusService::LocalGooseCycleStimulusService(
    LocalGooseCycleStimulusStore& stimulus_store,
    LocalGooseCycleStore& cycle_store,
    gaudere::work::TaskStore& task_store)
    : stimulus_store_(stimulus_store),
      cycle_store_(cycle_store),
      task_store_(task_store)
{
}

LocalGooseCycleStimulusServiceStep
LocalGooseCycleStimulusService::accept_explicit_recheck(
    const std::string& source_id,
    const std::int64_t accepted_at_ms)
{
    try {
        if (accepted_at_ms < 0) {
            return {ServiceResult::invalid, {}, {},
                    "stimulus acceptance timestamp is negative"};
        }

        if (const auto existing = stimulus_store_.find_by_source(source_id)) {
            return {ServiceResult::duplicate, existing,
                    cycle_store_.find(local_goose_cycle_scope), {}};
        }

        const auto cursor = cycle_store_.find(local_goose_cycle_scope);
        if (!cursor) {
            return {ServiceResult::unavailable, {}, {},
                    "Local Goose cycle sidecar is unseeded"};
        }
        if (cursor->state != CycleState::dormant
            || cursor->generation < 2) {
            return {ServiceResult::invalid, {}, cursor,
                    "explicit local recheck requires dormant generation >= 2"};
        }

        std::string predecessor_detail;
        if (!canonical_predecessor(
                *cursor, task_store_, predecessor_detail)) {
            return {ServiceResult::invalid, {}, cursor,
                    std::move(predecessor_detail)};
        }

        const auto stimulus = make_explicit_local_recheck_stimulus(
            source_id, accepted_at_ms,
            cursor->revision, cursor->generation);
        const auto write = stimulus_store_.append(stimulus);
        switch (write.result) {
        case StimulusStoreResult::accepted:
            return {ServiceResult::accepted, write.stimulus, cursor, {}};
        case StimulusStoreResult::duplicate:
            return {ServiceResult::duplicate, write.stimulus, cursor, {}};
        case StimulusStoreResult::conflict:
            return {ServiceResult::conflict, write.stimulus, cursor,
                    write.detail};
        case StimulusStoreResult::invalid:
            return {ServiceResult::invalid, write.stimulus, cursor,
                    write.detail};
        case StimulusStoreResult::unavailable:
            return {ServiceResult::unavailable, write.stimulus, cursor,
                    write.detail};
        }
    } catch (const std::exception& error) {
        return {ServiceResult::unavailable, {}, {}, error.what()};
    }
    return {ServiceResult::invalid, {}, {}, "unknown stimulus store result"};
}

LocalGooseCycleStimulusServiceStep
LocalGooseCycleStimulusService::reconcile(
    const std::string& stimulus_id,
    const std::int64_t observed_at_ms)
{
    try {
        if (observed_at_ms < 0) {
            return {ServiceResult::invalid, {}, {},
                    "stimulus observation timestamp is negative"};
        }

        const auto found = stimulus_store_.find(stimulus_id);
        if (!found) {
            return {ServiceResult::invalid, {}, {},
                    "Local Goose cycle stimulus not found"};
        }
        if (found->status != StimulusStatus::accepted) {
            return {terminal_result(found->status), found,
                    cycle_store_.find(local_goose_cycle_scope), {}};
        }
        const Stimulus accepted = *found;

        if (observed_at_ms < accepted.accepted_at_ms) {
            return manual_review(
                stimulus_store_, accepted, accepted.accepted_at_ms,
                "worker clock is before durable stimulus acceptance timestamp",
                cycle_store_.find(local_goose_cycle_scope));
        }

        auto cursor = cycle_store_.find(local_goose_cycle_scope);
        if (!cursor) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "Local Goose cycle cursor disappeared after stimulus acceptance",
                {});
        }

        std::string recovery_detail;
        if (exact_rearmed_cursor(
                *cursor, accepted, task_store_, recovery_detail)) {
            return terminalize(
                stimulus_store_, accepted, StimulusStatus::consumed,
                observed_at_ms, cursor->revision, {}, cursor);
        }

        if (accepted.target_cycle_revision
                < std::numeric_limits<std::uint64_t>::max()
            && cursor->revision == accepted.target_cycle_revision + 1
            && cursor->generation == accepted.target_cycle_generation) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "successor Local Goose cycle cursor differs from exact stimulus re-arm",
                cursor);
        }

        if (cursor->revision != accepted.target_cycle_revision
            || cursor->generation != accepted.target_cycle_generation) {
            return supersede(
                stimulus_store_, accepted, observed_at_ms,
                "target Local Goose cycle cursor changed before stimulus consumption",
                cursor);
        }

        if (cursor->state != CycleState::dormant) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "target Local Goose cycle state changed without revision advance",
                cursor);
        }

        std::string predecessor_detail;
        if (!canonical_predecessor(
                *cursor, task_store_, predecessor_detail)) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                predecessor_detail, cursor);
        }

        if (cursor->revision
            == std::numeric_limits<std::uint64_t>::max()) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "Local Goose cycle cursor revision cannot advance",
                cursor);
        }

        CycleCursor scheduled = *cursor;
        ++scheduled.revision;
        scheduled.state = CycleState::scheduled;
        scheduled.due_at_ms = accepted.accepted_at_ms;

        if (!valid_local_goose_cycle_transition(*cursor, scheduled)) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "dormant Local Goose cycle cannot transition to scheduled",
                cursor);
        }

        const auto cycle_write = cycle_store_.replace(*cursor, scheduled);
        if (cycle_write.result == CycleStoreResult::unavailable) {
            return {ServiceResult::unavailable, accepted,
                    cycle_write.cursor,
                    cycle_write.detail};
        }
        if (cycle_write.result == CycleStoreResult::invalid) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                cycle_write.detail.empty()
                    ? "Local Goose cycle re-arm transition was rejected"
                    : cycle_write.detail,
                cycle_write.cursor ? cycle_write.cursor : cursor);
        }

        if (cycle_write.result == CycleStoreResult::accepted
            || cycle_write.result == CycleStoreResult::duplicate) {
            const auto persisted = cycle_write.cursor
                ? cycle_write.cursor
                : std::optional<CycleCursor>{scheduled};
            std::string exact_detail;
            if (!exact_rearmed_cursor(
                    *persisted, accepted, task_store_, exact_detail)) {
                return manual_review(
                    stimulus_store_, accepted, observed_at_ms,
                    exact_detail.empty()
                        ? "persisted Local Goose cycle re-arm differs"
                        : exact_detail,
                    persisted);
            }
            return terminalize(
                stimulus_store_, accepted, StimulusStatus::consumed,
                observed_at_ms, persisted->revision, {}, persisted);
        }

        cursor = cycle_store_.find(local_goose_cycle_scope);
        if (!cursor) {
            return manual_review(
                stimulus_store_, accepted, observed_at_ms,
                "Local Goose cycle cursor disappeared after re-arm conflict",
                {});
        }
        recovery_detail.clear();
        if (exact_rearmed_cursor(
                *cursor, accepted, task_store_, recovery_detail)) {
            return terminalize(
                stimulus_store_, accepted, StimulusStatus::consumed,
                observed_at_ms, cursor->revision, {}, cursor);
        }
        return supersede(
            stimulus_store_, accepted, observed_at_ms,
            "Local Goose cycle cursor changed during stimulus re-arm",
            cursor);
    } catch (const std::exception& error) {
        return {ServiceResult::unavailable, {}, {}, error.what()};
    }
}

} // namespace gaudere_agent
