#ifndef GAUDERE_AGENT_LOCAL_GOOSE_V2_BRIDGE_PROOF_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_V2_BRIDGE_PROOF_HPP

#include "LocalGooseSettledInspection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// Read-only evidence of an already-dormant V1 cursor. Neither this status nor
// its digest can authorize a V2 Task, a cursor migration, or a new wake.
enum class LocalGooseV2BridgeStatus {
    verified_dormant_no_authority,
    not_applicable,
    no_predecessor,
    snapshot_changed,
    noncanonical,
    unavailable
};

struct LocalGooseV2BridgeEvidence {
    std::uint64_t cursor_revision = 0;
    std::uint64_t next_generation = 0;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    // Digest only. Never expose raw model assessment/reason/request text.
    std::string bounded_memory_sha256;
    std::size_t bounded_memory_entries = 0;
    bool earlier_generations_omitted = false;
};

struct LocalGooseV2BridgeInspection {
    LocalGooseV2BridgeStatus status = LocalGooseV2BridgeStatus::unavailable;
    std::optional<LocalGooseV2BridgeEvidence> evidence;
};

// Inert, bounded inspection. Both callbacks must be read-only and backed by
// the sole Agent owner thread, not another process opening production state.db.
// read_cursor must target the fixed canonical V1 scope; read_task must do only
// exact-ID lookups. active_model_sha256 comes from pinned trusted config.
// At most 2 cursor reads and 5 exact Task reads (anchor, predecessor, 3 links).
// No Task construction, submission, storage, wake, provider, CAS or migration.
// A later activation would need its own durable single-writer authority gate.
[[nodiscard]] LocalGooseV2BridgeInspection
inspect_local_goose_v2_dormant_bridge(
    const LocalGooseCursorRead& read_cursor,
    const LocalGooseTaskRead& read_task,
    const std::string& active_model_sha256) noexcept;

} // namespace gaudere_agent
#endif
