#ifndef GAUDERE_AGENT_LOCAL_CONTINUITY_OBSERVATION_HPP
#define GAUDERE_AGENT_LOCAL_CONTINUITY_OBSERVATION_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_continuity_observation_schema =
    "gaudere.continuity.local-observation.v1";
inline constexpr const char* local_continuity_observation_scope =
    "continuity.local-observation.v1";
inline constexpr const char* local_continuity_observation_task_kind =
    "continuity.local-observation.v1";
inline constexpr const char* local_continuity_observation_content_type =
    "application/vnd.gaudere.continuity-local-observation+json";
inline constexpr const char* local_continuity_observation_task_prefix =
    "continuity.local-observation.v1:";
inline constexpr const char* local_continuity_observation_identity_schema =
    "gaudere.continuity.local-observation.identity.v1";

/** Immutable fields that identify one admitted observation opportunity. */
struct LocalContinuityObservationOpportunity {
    std::uint32_t generation = 0;
    std::int64_t due_at_ms = 0;
    std::optional<std::string> predecessor_observation_task_id;
    std::optional<std::string> predecessor_observation_result_sha256;
    std::string anchor_checkpoint_task_id;
    std::string anchor_checkpoint_result_sha256;
};

struct LocalContinuityObservationFacts {
    std::uint32_t generation = 0;
    std::int64_t due_at_ms = 0;
    std::int64_t captured_at_ms = 0;
    std::optional<std::string> predecessor_observation_task_id;
    std::optional<std::string> predecessor_observation_result_sha256;

    // Immutable seed anchor. The result hash is included in opportunity identity so
    // a Task cannot be silently rebound to different checkpoint bytes.
    std::string anchor_checkpoint_task_id;
    std::string anchor_checkpoint_result_sha256;

    // Bounded durable evidence only. These values are obtainable through existing
    // exact IDs/scopes; no global Task/Action/Wake enumeration is required.
    std::string provider_scope;
    std::uint64_t provider_total = 0;
    std::uint64_t provider_limit = 0;
    std::string predecessor_provider_action_id;
    std::string audited_provider_action_id;
    std::string historical_wake_scope;
    std::string historical_wake_sha256;
};

struct LocalContinuityObservationInspection {
    bool eligible = false;
    std::string detail;
    LocalContinuityObservationFacts facts;
    std::string canonical_payload;
};

[[nodiscard]] LocalContinuityObservationOpportunity
local_continuity_observation_opportunity(
    const LocalContinuityObservationFacts& facts) noexcept;

/** Canonical identity bytes for one opportunity, independent of captured facts. */
[[nodiscard]] std::string local_continuity_observation_opportunity_identity(
    const LocalContinuityObservationOpportunity& opportunity);

/** Compatibility overload for a fully captured observation. */
[[nodiscard]] std::string local_continuity_observation_opportunity_identity(
    const LocalContinuityObservationFacts& facts);

/** Deterministic Task ID that may be reserved before evidence capture. */
[[nodiscard]] std::string local_continuity_observation_task_id(
    const LocalContinuityObservationOpportunity& opportunity);

[[nodiscard]] gaudere::work::Task make_local_continuity_observation_task(
    const LocalContinuityObservationFacts& facts);

[[nodiscard]] LocalContinuityObservationInspection inspect_local_continuity_observation_payload(
    const std::string& payload) noexcept;

[[nodiscard]] LocalContinuityObservationInspection inspect_local_continuity_observation_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] bool canonical_local_continuity_observation_success(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
