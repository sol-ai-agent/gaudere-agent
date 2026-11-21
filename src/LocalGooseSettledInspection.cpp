#include "LocalGooseSettledInspection.hpp"
#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <tuple>

namespace gaudere_agent {
namespace {
using Task = gaudere::work::Task;
using Status = LocalGooseSettledStatus;

bool digest_shape(const std::string& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool same_cursor(const LocalGooseCycleCursor& a, const LocalGooseCycleCursor& b)
{
    return std::tie(a.scope, a.revision, a.generation, a.state,
                    a.anchor_observation_task_id, a.anchor_observation_result_sha256,
                    a.predecessor_task_id, a.predecessor_result_sha256,
                    a.due_at_ms, a.captured_at_ms, a.current_task_id, a.blocked_reason)
        == std::tie(b.scope, b.revision, b.generation, b.state,
                    b.anchor_observation_task_id, b.anchor_observation_result_sha256,
                    b.predecessor_task_id, b.predecessor_result_sha256,
                    b.due_at_ms, b.captured_at_ms, b.current_task_id, b.blocked_reason);
}

bool bounded(const Task& task, std::size_t input_limit, std::size_t output_limit)
{
    return task.result && task.id.size() <= 128 &&
        task.input.size() <= input_limit &&
        task.result->output.size() <= output_limit;
}

bool cycle_evidence(const Task& task, const std::string& id,
                    const std::string& digest, std::uint64_t generation,
                    const LocalGooseCycleCursor& cursor, const std::string& model)
{
    // Bounds match the existing LocalGooseCycle / LocalGooseCognition contracts.
    // Explicit raw equality also holds before the canonical-success hardening lands.
    if (!bounded(task, 48 * 1024, 16 * 1024) || task.id != id ||
        !canonical_local_goose_cycle_success(task) ||
        sha256_hex(task.result->output) != digest)
        return false;
    const auto facts = inspect_local_goose_cycle_task(task);
    const auto decision = inspect_local_goose_decision(task.result->output);
    return facts.eligible && decision.eligible &&
        task.result->output == decision.decision.canonical_json &&
        facts.generation == generation &&
        facts.anchor_observation_task_id == cursor.anchor_observation_task_id &&
        facts.anchor_observation_result_sha256 == cursor.anchor_observation_result_sha256 &&
        facts.model_sha256 == model;
}

LocalGooseSettledInspection fail(Status status)
{
    return {status, std::nullopt};
}
} // namespace

LocalGooseSettledInspection inspect_local_goose_settled_decision(
    const LocalGooseCursorRead& read_cursor,
    const LocalGooseTaskRead& read_task,
    const std::string& active_model_sha256) noexcept
{
    try {
        const auto cursor = read_cursor();
        if (!cursor) return fail(Status::unavailable);
        if (!valid_local_goose_cycle_cursor(*cursor))
            return fail(Status::noncanonical);
        if (cursor->generation < 2) return fail(Status::no_predecessor);
        if (cursor->state != LocalGooseCycleState::dormant)
            return fail(Status::not_applicable);
        if (!digest_shape(active_model_sha256))
            return fail(Status::unavailable);

        const auto predecessor = read_task(*cursor->predecessor_task_id);
        if (!predecessor) return fail(Status::unavailable);
        const auto generation = cursor->generation - 1;
        if (!cycle_evidence(*predecessor, *cursor->predecessor_task_id,
                            *cursor->predecessor_result_sha256, generation,
                            *cursor, active_model_sha256))
            return fail(Status::noncanonical);

        const auto anchor = read_task(cursor->anchor_observation_task_id);
        if (!anchor) return fail(Status::unavailable);
        if (!bounded(*anchor, 32 * 1024, 32 * 1024) ||
            anchor->id != cursor->anchor_observation_task_id ||
            !canonical_local_continuity_observation_success(*anchor) ||
            sha256_hex(anchor->result->output) != cursor->anchor_observation_result_sha256 ||
            inspect_local_continuity_observation_task(*anchor).facts.generation != 3)
            return fail(Status::noncanonical);

        const auto facts = inspect_local_goose_cycle_task(*predecessor);
        if (generation > 1) {
            if (!facts.predecessor) return fail(Status::noncanonical);
            const auto& link = *facts.predecessor;
            const auto previous = read_task(link.task_id);
            if (!previous) return fail(Status::unavailable);
            if (!cycle_evidence(*previous, link.task_id, link.result_sha256,
                                generation - 1, *cursor, active_model_sha256) ||
                previous->result->output != link.decision.canonical_json)
                return fail(Status::noncanonical);
        }
        const auto decision = inspect_local_goose_decision(predecessor->result->output);
        if (!decision.eligible || decision.decision.next_wake_after_ms ||
            (decision.decision.decision != "idle" &&
             decision.decision.decision != "request_openai"))
            return fail(Status::noncanonical);

        const auto after = read_cursor();
        if (!after || !same_cursor(*cursor, *after))
            return fail(Status::snapshot_changed);
        if (!valid_local_goose_cycle_cursor(*after))
            return fail(Status::noncanonical);

        return {Status::verified, LocalGooseSettledEvidence{
            cursor->revision, cursor->generation, generation,
            decision.decision.decision == "idle" ? LocalGooseSettledDecision::idle
                : LocalGooseSettledDecision::request_openai,
            std::nullopt, *cursor->predecessor_result_sha256}};
    } catch (...) {
        // Never expose exception text or the raw model decision.
        return fail(Status::unavailable);
    }
}
} // namespace gaudere_agent
