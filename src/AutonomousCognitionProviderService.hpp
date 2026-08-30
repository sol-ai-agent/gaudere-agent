#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_SERVICE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_SERVICE_HPP

#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionPulseService.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

struct AutonomousCognitionProviderServiceStep {
    bool healthy = true;
    bool monitoring = false;
    bool executed = false;
    std::optional<gaudere::scheduling::wake::Scheduler::TimePoint> next_at;
    AutonomousCognitionProviderGateObservation gate;
    std::optional<AutonomousCognitionPulseServiceStep> settlement;
    std::string detail;
};

/**
 * Persistent-service bridge for an explicitly enabled pulse provider authority.
 *
 * This class does not choose a Task and does not own provider effects. The read-only
 * gate must first name the exact pulse-prepared Task; TaskExecutor plus the supplied
 * handler then cross the already-proven durable provider Action/budget boundary.
 * After a completed Task lifecycle, the pulse service alone settles/re-arms the
 * cursor. Waiting budget states may schedule a future observation but never mutate
 * the pulse cursor or invoke the handler.
 */
class AutonomousCognitionProviderService final {
public:
    using Now = std::function<gaudere::scheduling::wake::Scheduler::TimePoint()>;

    AutonomousCognitionProviderService(
        AutonomousCognitionProviderGate& gate,
        TaskExecutor& executor,
        TaskHandler& handler,
        AutonomousCognitionPulseService& pulse_service,
        gaudere::scheduling::wake::Scheduler& scheduler,
        Now now);

    [[nodiscard]] AutonomousCognitionProviderServiceStep step(
        const AutonomousCognitionPulseCursor& cursor);

private:
    AutonomousCognitionProviderGate& gate_;
    TaskExecutor& executor_;
    TaskHandler& handler_;
    AutonomousCognitionPulseService& pulse_service_;
    gaudere::scheduling::wake::Scheduler& scheduler_;
    Now now_;
};

} // namespace gaudere_agent

#endif
