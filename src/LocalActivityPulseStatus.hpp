#ifndef GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_STATUS_HPP
#define GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_STATUS_HPP

#include <string>

namespace gaudere_agent {

inline constexpr const char* local_activity_pulse_status_schema =
    "gaudere.continuity.local-observation-status.v1";

struct LocalActivityPulseStatusInspection {
    bool eligible = false;
    std::string canonical_json;
    std::string detail;
};

/**
 * Strict read-only status inspection for the inert local continuity pulse.
 *
 * `state_path` is the already-existing Core SQLite state database. `sidecar_path`
 * may be absent (reported as unseeded) or must pass the existing strict sidecar
 * inspection. The function never creates schema, recovers Tasks, advances the
 * pulse, mutates Scheduler/WakeIntent/Action/Budget state, or acquires provider
 * authority.
 */
[[nodiscard]] LocalActivityPulseStatusInspection inspect_local_activity_pulse_status(
    const std::string& state_path,
    const std::string& sidecar_path,
    bool enabled_source_intent) noexcept;

} // namespace gaudere_agent

#endif
