#ifndef GAUDERE_AGENT_CANONICAL_COGNITION_DECISION_HPP
#define GAUDERE_AGENT_CANONICAL_COGNITION_DECISION_HPP

#include <optional>
#include <string>

namespace gaudere_agent {

struct CanonicalCognitionDecision {
    bool eligible = false;
    std::string decision;
    std::string reason;
    std::optional<std::string> objective;
    std::string canonical_output;
    std::string detail;
};

/** Strict read-only parser for the durable cognition decision dialect. */
[[nodiscard]] CanonicalCognitionDecision inspect_canonical_cognition_decision(
    const std::string& output) noexcept;

} // namespace gaudere_agent

#endif
