#include "AutonomousCognitionProviderService.hpp"

#include <stdexcept>
#include <utility>

namespace gaudere_agent {

AutonomousCognitionProviderService::AutonomousCognitionProviderService(
    AutonomousCognitionProviderGate& gate,
    TaskExecutor& executor,
    TaskHandler& handler,
    AutonomousCognitionPulseService& pulse_service,
    gaudere::scheduling::wake::Scheduler& scheduler)
    : gate_(gate), executor_(executor), handler_(handler),
      pulse_service_(pulse_service), scheduler_(scheduler)
{
}

AutonomousCognitionProviderServiceStep
AutonomousCognitionProviderService::step(
    const AutonomousCognitionPulseCursor& cursor)
{
    AutonomousCognitionProviderServiceStep result;
    result.gate = gate_.evaluate(cursor);

    using GateResult = AutonomousCognitionProviderGateResult;
    switch (result.gate.result) {
    case GateResult::eligible: {
        if (!result.gate.task_id) {
            result.healthy = false;
            result.detail = "eligible autonomous provider gate has no Task id";
            return result;
        }
        const auto execution = executor_.execute(
            *result.gate.task_id, "autonomous-pulse-provider", handler_);
        if (execution != ExecuteResult::completed) {
            result.healthy = false;
            result.detail = "autonomous pulse provider Task did not complete its lifecycle";
            return result;
        }
        result.executed = true;
        result.settlement = pulse_service_.step();
        result.healthy = result.settlement->plan.healthy;
        result.monitoring = result.settlement->plan.monitoring;
        result.next_at = result.settlement->plan.next_at;
        result.detail = result.settlement->plan.detail;
        return result;
    }

    case GateResult::waiting:
        result.detail = result.gate.detail;
        if (result.gate.retry_at) {
            static_cast<void>(scheduler_.request_at(*result.gate.retry_at));
            result.monitoring = true;
            result.next_at = result.gate.retry_at;
        }
        return result;

    case GateResult::dormant:
        result.detail = result.gate.detail;
        return result;

    case GateResult::blocked:
        result.healthy = false;
        result.detail = result.gate.detail;
        return result;

    case GateResult::unavailable:
        result.healthy = false;
        result.detail = result.gate.detail;
        return result;
    }

    throw std::logic_error("unknown autonomous cognition provider gate result");
}

} // namespace gaudere_agent
