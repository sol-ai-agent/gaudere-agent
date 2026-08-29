#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_SERVICE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_SERVICE_HPP

#include "AutonomousCognitionPulse.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

struct AutonomousCognitionPulseServicePlan {
    bool healthy = true;
    bool monitoring = false;
    std::optional<gaudere::scheduling::wake::Scheduler::TimePoint> next_at;
    std::string detail;
};

/**
 * Derive the next provider-free service observation from one pulse observation.
 *
 * No durable state is mutated here. A budget snapshot is required only when the
 * pulse reported budget_blocked. Prepared/waiting cognition deliberately has no
 * automatic deadline: provider execution remains a separate authority and requires
 * a later stopped-state one-shot/restart cycle.
 */
[[nodiscard]] AutonomousCognitionPulseServicePlan
plan_autonomous_cognition_pulse_service(
    const AutonomousCognitionPulseObservation& observation,
    const std::optional<gaudere::budget::Snapshot>& budget,
    gaudere::scheduling::wake::Scheduler::TimePoint now,
    const gaudere::budget::Policy& policy) noexcept;

struct AutonomousCognitionPulseServiceStep {
    AutonomousCognitionPulseObservation observation;
    AutonomousCognitionPulseServicePlan plan;
};

/** Provider-free bridge from one durable pulse to the existing service Scheduler. */
class AutonomousCognitionPulseService {
public:
    using Now = std::function<gaudere::scheduling::wake::Scheduler::TimePoint()>;

    AutonomousCognitionPulseService(
        AutonomousCognitionPulse& pulse,
        gaudere::budget::Store& budget_store,
        gaudere::scheduling::wake::Scheduler& scheduler,
        Now now);

    [[nodiscard]] AutonomousCognitionPulseServiceStep step();

private:
    AutonomousCognitionPulse& pulse_;
    gaudere::budget::Store& budget_store_;
    gaudere::scheduling::wake::Scheduler& scheduler_;
    Now now_;
};

} // namespace gaudere_agent

#endif
