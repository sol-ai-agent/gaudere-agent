#ifndef GAUDERE_AGENT_CONTINUITY_DELTA_CHECKPOINT_INSPECTION_HPP
#define GAUDERE_AGENT_CONTINUITY_DELTA_CHECKPOINT_INSPECTION_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {

/**
 * Strict provider-free read-only view of one already-succeeded continuity
 * checkpoint. It exposes only bounded identities/scopes required by later local
 * continuity observers; it owns no store, provider, network or mutation authority.
 */
struct ContinuityDeltaCheckpointInspection {
    bool eligible = false;
    std::string detail;

    std::string checkpoint_result_sha256;
    std::string audited_task_id;
    std::string predecessor_task_id;
    std::string audited_provider_action_id;
    std::string audited_provider_action_key;
    std::string predecessor_provider_action_id;
    std::string predecessor_provider_action_key;
    std::string provider_scope;
    std::uint64_t provider_total = 0;
    std::string historical_wake_scope;
    std::string historical_wake_sha256;
    std::string historical_wake_canonical;
};

[[nodiscard]] ContinuityDeltaCheckpointInspection
inspect_succeeded_continuity_delta_checkpoint(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
