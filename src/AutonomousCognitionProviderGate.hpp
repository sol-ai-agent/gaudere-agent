#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_GATE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_GATE_HPP

#include "AutonomousCognitionPulseStore.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class AutonomousCognitionProviderGateResult {
    eligible,
    waiting,
    dormant,
    blocked,
    unavailable
};

struct AutonomousCognitionProviderGateObservation {
    AutonomousCognitionProviderGateResult result =
        AutonomousCognitionProviderGateResult::unavailable;
    std::optional<std::string> task_id;
    std::optional<gaudere::work::TimePoint> retry_at;
    std::string detail;
};

/**
 * Read-only authority gate between one pulse-prepared cognition opportunity and
 * the existing provider effect boundary.
 *
 * evaluate() never saves a Task or Action, never consumes budget, never changes
 * the pulse cursor, and owns no Provider, secret, network or service wiring.
 */
class AutonomousCognitionProviderGate {
public:
    using Now = std::function<gaudere::work::TimePoint()>;

    AutonomousCognitionProviderGate(
        std::string state_path,
        gaudere::work::TaskStore& task_store,
        gaudere::budget::Store& budget_store,
        gaudere::scheduling::wake::ActionStore& action_store,
        Now now);

    [[nodiscard]] AutonomousCognitionProviderGateObservation evaluate(
        const AutonomousCognitionPulseCursor& cursor) const;

private:
    std::string state_path_;
    gaudere::work::TaskStore& task_store_;
    gaudere::budget::Store& budget_store_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    Now now_;
};

} // namespace gaudere_agent

#endif
