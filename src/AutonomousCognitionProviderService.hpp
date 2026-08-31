#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_SERVICE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PROVIDER_SERVICE_HPP

#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionPulseService.hpp"
#include "CurrentCognitionHandler.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

struct AutonomousCognitionProviderServiceStep {
    bool healthy = true;
    bool monitoring = false;
    bool provider_executed = false;
    std::optional<gaudere::scheduling::wake::Scheduler::TimePoint> next_at;
    std::optional<std::string> task_id;
    std::string detail;
};

/**
 * Persistent-service bridge for the explicit autonomous pulse provider authority.
 *
 * The pulse remains the sole owner of time/context/Task preparation and settlement.
 * The read-only gate names at most one provider-authorized Task. TaskExecutor and
 * the borrowed CurrentCognitionHandler then cross the already-existing provider
 * effect boundary. This class creates no provider, secret, shell, Drive, GitHub,
 * WakeIntent or successor authority of its own.
 */
class AutonomousCognitionProviderService {
public:
    using Now = std::function<gaudere::scheduling::wake::Scheduler::TimePoint()>;

    AutonomousCognitionProviderService(
        AutonomousCognitionPulseService& pulse_service,
        AutonomousCognitionProviderGate& provider_gate,
        TaskExecutor& executor,
        CurrentCognitionHandler& cognition_handler,
        gaudere::work::TaskStore& task_store,
        gaudere::scheduling::wake::Scheduler& scheduler,
        Now now);

    [[nodiscard]] AutonomousCognitionProviderServiceStep step();

private:
    AutonomousCognitionPulseService& pulse_service_;
    AutonomousCognitionProviderGate& provider_gate_;
    TaskExecutor& executor_;
    CurrentCognitionHandler& cognition_handler_;
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::Scheduler& scheduler_;
    Now now_;
};

} // namespace gaudere_agent

#endif
