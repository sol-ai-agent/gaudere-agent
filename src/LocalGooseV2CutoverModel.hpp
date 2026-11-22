#ifndef GAUDERE_AGENT_LOCAL_GOOSE_V2_CUTOVER_MODEL_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_V2_CUTOVER_MODEL_HPP

#include "LocalGooseCycleStore.hpp"
#include "LocalGooseCycleV2.hpp"
#include "LocalGooseV2BridgeProof.hpp"

#include <gaudere/work/Task.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gaudere_agent {

// SOURCE-ONLY: neither this model nor any state returned by it has authority
// to write a database, approve a migration, submit a Task, or wake a cycle.
inline constexpr const char* local_goose_v2_cutover_candidate_schema =
    "gaudere.cognition.local-goose-v2-cutover-candidate.v1";
inline constexpr const char* local_goose_v2_cutover_candidate_prefix =
    "cognition.local-goose-v2-cutover-candidate.v1:";

// Frozen preview binding the exact dormant V1 observation to one hypothetical
// V2 Task. A digest-shaped ID is NOT an approval token.
struct LocalGooseV2CutoverCandidate {
    bool eligible = false;
    std::string canonical_json;
    std::string candidate_id;
    std::string v2_task_id;
    std::string v2_input_sha256;
    std::string detail;
};

// Pure, read-only consistency check. The caller must independently establish
// the authenticity/owner-thread provenance of all three inputs. Even a valid
// candidate grants NO permission to migrate or run.
[[nodiscard]] LocalGooseV2CutoverCandidate make_local_goose_v2_cutover_candidate(
    const LocalGooseCycleCursor& dormant_v1_cursor,
    const LocalGooseV2BridgeInspection& bridge,
    const gaudere::work::Task& frozen_v2_preview,
    const std::string& pinned_model_sha256) noexcept;

// Simulation of future durable ledger transitions, NOT a durable ledger.
// Only a future transactionally fenced store may attest that any of these
// events actually occurred. In particular, the external epoch and publish
// receipt supplied here are untrusted caller claims.
enum class LocalGooseV2CutoverPhase {
    proposal,       // no V1 fence, no V2 Task
    fenced,         // future owner-store CAS; V1 writes must be blocked
    frozen,         // exact V2 ID committed to future durable ledger
    published,      // exact V2 Task committed to TaskStore
    aborted,        // only before the frozen point, do not release on ambiguity
    quarantined     // irreversible containment, NOT automatic V1 reactivation
};

enum class LocalGooseV2CutoverEvent {
    fence_v1,
    freeze_identity,
    confirm_publish,
    abort_pre_freeze,
    quarantine
};

enum class LocalGooseV2CutoverResult {
    accepted, duplicate, conflict, invalid
};

struct LocalGooseV2CutoverRecord {
    std::string candidate_id;
    std::string v2_task_id;
    std::string v2_input_sha256;
    std::uint64_t revision = 0;
    std::uint64_t fence_epoch = 0;
    LocalGooseV2CutoverPhase phase = LocalGooseV2CutoverPhase::proposal;
};

struct LocalGooseV2CutoverStep {
    LocalGooseV2CutoverResult result = LocalGooseV2CutoverResult::invalid;
    std::optional<LocalGooseV2CutoverRecord> record;
};

[[nodiscard]] bool valid_local_goose_v2_cutover_record(
    const LocalGooseV2CutoverRecord& record) noexcept;

[[nodiscard]] std::optional<LocalGooseV2CutoverRecord>
initial_local_goose_v2_cutover_record(
    const LocalGooseV2CutoverCandidate& candidate) noexcept;

// A pure state transition calculator; there is NO I/O/transaction/authorization.
// The owner must eventually prove the fence and publish receipt transactionally.
// expected_revision simulates CAS; fence_epoch simulates owner fencing token.
// confirmed_task_id/input_sha256 apply only to confirm_publish.
[[nodiscard]] LocalGooseV2CutoverStep simulate_local_goose_v2_cutover(
    const LocalGooseV2CutoverRecord& current,
    LocalGooseV2CutoverEvent event,
    std::uint64_t expected_revision,
    std::uint64_t fence_epoch,
    std::string_view confirmed_task_id = {},
    std::string_view confirmed_input_sha256 = {}) noexcept;

} // namespace gaudere_agent
#endif
