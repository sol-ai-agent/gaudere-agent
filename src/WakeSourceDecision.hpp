#ifndef GAUDERE_AGENT_WAKE_SOURCE_DECISION_HPP
#define GAUDERE_AGENT_WAKE_SOURCE_DECISION_HPP

#include <gaudere/work/Task.hpp>

#include <chrono>
#include <string>

namespace gaudere_agent {

inline constexpr const char* bounded_reflection_wake_scope =
    "cognition.reflect.wake.v0";

/** Exact canonical validation shared by explicit wake acceptance/observability and
 * future read-only consumers of already-durable wake evidence.
 */
struct WakeSourceDecision {
    bool eligible = false;
    std::chrono::seconds delay{0};
    std::string reason;
    std::string canonical_output;
    std::string detail;
};

[[nodiscard]] WakeSourceDecision inspect_wake_source_decision(
    const gaudere::work::Task& task);

} // namespace gaudere_agent

#endif
