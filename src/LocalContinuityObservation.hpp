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

struct LocalContinuityObservationFacts {
    std::uint32_t generation = 0;
    std::int64_t due_at_ms = 0;
    std::int64_t captured_at_ms = 0;
    std::optional<std::string> predecessor_observation_task_id;
    std::optional<std::string> predecessor_observation_result_sha256;
    std::string anchor_checkpoint_task_id;
    std::uint64_t provider_total = 0;
    std::uint64_t provider_limit = 0;
    std::uint64_t actions_total = 0;
    std::uint64_t actions_confirmed = 0;
    std::uint64_t wake_total = 0;
    std::uint64_t wake_fired = 0;
    std::uint64_t checkpoint_count = 0;
    std::string latest_checkpoint_task_id;
};

struct LocalContinuityObservationInspection {
    bool eligible = false;
    std::string detail;
    LocalContinuityObservationFacts facts;
    std::string canonical_payload;
};

[[nodiscard]] std::string local_continuity_observation_opportunity_identity(
    const LocalContinuityObservationFacts& facts);

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
