#include "LocalGooseV2BridgeProof.hpp"

#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "LocalGooseCycleMemory.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <cstddef>
#include <tuple>

namespace gaudere_agent {
namespace {
using Task = gaudere::work::Task;
using Cursor = LocalGooseCycleCursor;
using Status = LocalGooseV2BridgeStatus;

LocalGooseV2BridgeInspection fail(Status status)
{
    return {status, std::nullopt};
}

bool digest_shape(const std::string& value) noexcept
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
        [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool same_cursor(const Cursor& a, const Cursor& b)
{
    // Compare all fields, not just revision: a buggy/compromised reader may
    // return a different state with the same revision.
    return std::tie(a.scope, a.revision, a.generation, a.state,
                    a.anchor_observation_task_id, a.anchor_observation_result_sha256,
                    a.predecessor_task_id, a.predecessor_result_sha256,
                    a.due_at_ms, a.captured_at_ms, a.current_task_id, a.blocked_reason)
        == std::tie(b.scope, b.revision, b.generation, b.state,
                    b.anchor_observation_task_id, b.anchor_observation_result_sha256,
                    b.predecessor_task_id, b.predecessor_result_sha256,
                    b.due_at_ms, b.captured_at_ms, b.current_task_id, b.blocked_reason);
}

bool bounded(const Task& task, std::size_t input, std::size_t output)
{
    return task.result && task.id.size() <= 128
        && task.input.size() <= input && task.result->output.size() <= output;
}
} // namespace

LocalGooseV2BridgeInspection inspect_local_goose_v2_dormant_bridge(
    const LocalGooseCursorRead& read_cursor,
    const LocalGooseTaskRead& read_task,
    const std::string& active_model_sha256) noexcept
{
    try {
        const auto cursor = read_cursor();
        if (!cursor) return fail(Status::unavailable);
        if (!valid_local_goose_cycle_cursor(*cursor))
            return fail(Status::noncanonical);
        // A dormant cycle is explicitly NOT an implicit V2 opportunity.
        // Scheduled/prepared/blocked V1 records cannot be appropriated either.
        if (cursor->state != LocalGooseCycleState::dormant)
            return fail(Status::not_applicable);
        if (cursor->generation < 2)
            return fail(Status::no_predecessor);
        if (!digest_shape(active_model_sha256))
            return fail(Status::unavailable);

        const auto predecessor = read_task(*cursor->predecessor_task_id);
        if (!predecessor) return fail(Status::unavailable);
        if (!bounded(*predecessor, 48 * 1024, 16 * 1024)
            || predecessor->id != *cursor->predecessor_task_id
            || !canonical_local_goose_cycle_success(*predecessor)
            || sha256_hex(predecessor->result->output)
                != *cursor->predecessor_result_sha256)
            return fail(Status::noncanonical);

        const auto facts = inspect_local_goose_cycle_task(*predecessor);
        if (!facts.eligible || facts.generation != cursor->generation - 1
            || facts.anchor_observation_task_id
                != cursor->anchor_observation_task_id
            || facts.anchor_observation_result_sha256
                != cursor->anchor_observation_result_sha256
            || facts.model_sha256 != active_model_sha256)
            return fail(Status::noncanonical);

        // A dormant V1 result has an explicit no-wake decision; no inference
        // or scheduling rule is synthesized from its untrusted prose.
        const auto decision = inspect_local_goose_decision(
            predecessor->result->output);
        if (!decision.eligible || decision.decision.next_wake_after_ms
            || (decision.decision.decision != "idle"
                && decision.decision.decision != "request_openai"))
            return fail(Status::noncanonical);

        const auto anchor = read_task(cursor->anchor_observation_task_id);
        if (!anchor) return fail(Status::unavailable);
        if (!bounded(*anchor, 32 * 1024, 32 * 1024)
            || anchor->id != cursor->anchor_observation_task_id
            || !canonical_local_continuity_observation_success(*anchor)
            || sha256_hex(anchor->result->output)
                != cursor->anchor_observation_result_sha256)
            return fail(Status::noncanonical);
        const auto anchor_facts =
            inspect_local_continuity_observation_task(*anchor);
        if (!anchor_facts.eligible || anchor_facts.facts.generation != 3)
            return fail(Status::noncanonical);

        // The predecessor is selected solely from the trusted cursor.
        // Subsequent lookups follow canonical predecessor links, max three.
        const auto memory = build_local_goose_cycle_memory(
            *predecessor,
            [&](const std::string& id) -> std::optional<Task> {
                return read_task(id);
            });
        if (memory.status == LocalGooseCycleMemoryStatus::unavailable)
            return fail(Status::unavailable);
        if (memory.status != LocalGooseCycleMemoryStatus::verified)
            return fail(Status::noncanonical);

        // Check the WHOLE fixed-scope snapshot after every Task lookup.
        // This is an observation gate, not a durable migration CAS/lease.
        const auto after = read_cursor();
        if (!after || !same_cursor(*cursor, *after))
            return fail(Status::snapshot_changed);
        if (!valid_local_goose_cycle_cursor(*after))
            return fail(Status::noncanonical);

        return {Status::verified_dormant_no_authority,
            LocalGooseV2BridgeEvidence{
                cursor->revision, cursor->generation,
                *cursor->predecessor_task_id,
                *cursor->predecessor_result_sha256,
                sha256_hex(memory.canonical_json),
                memory.included, memory.earlier_omitted
            }};
    } catch (...) {
        // No raw model text or provider/Task errors may escape this diagnostic.
        return fail(Status::unavailable);
    }
}
} // namespace gaudere_agent
