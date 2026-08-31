#include "AutonomousCognitionProviderService.hpp"

#include <gaudere/work/Task.hpp>

#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using GateResult = AutonomousCognitionProviderGateResult;
using PulseState = AutonomousCognitionPulseState;

AutonomousCognitionProviderServiceStep from_pulse_step(
    const AutonomousCognitionPulseServiceStep& step,
    std::string detail = {})
{
    if (detail.empty()) detail = step.plan.detail;
    return {step.plan.healthy, step.plan.monitoring, false,
            step.plan.next_at, {}, std::move(detail)};
}

} // namespace

AutonomousCognitionProviderService::AutonomousCognitionProviderService(
    AutonomousCognitionPulseService& pulse_service,
    AutonomousCognitionProviderGate& provider_gate,
    TaskExecutor& executor,
    CurrentCognitionHandler& cognition_handler,
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::Scheduler& scheduler,
    Now now)
    : pulse_service_(pulse_service), provider_gate_(provider_gate),
      executor_(executor), cognition_handler_(cognition_handler),
      task_store_(task_store), scheduler_(scheduler), now_(std::move(now))
{
    if (!now_)
        throw std::invalid_argument("autonomous provider service clock is required");
}

AutonomousCognitionProviderServiceStep
AutonomousCognitionProviderService::step()
{
    auto pulse_step = pulse_service_.step();
    if (!pulse_step.plan.healthy || !pulse_step.observation.cursor)
        return from_pulse_step(pulse_step);

    const auto& cursor = *pulse_step.observation.cursor;
    if (cursor.state != PulseState::prepared)
        return from_pulse_step(pulse_step);

    const auto gate = provider_gate_.evaluate(cursor);
    switch (gate.result) {
    case GateResult::eligible: {
        if (!gate.task_id || gate.task_id->empty())
            return {false, false, false, {}, {},
                    "eligible autonomous provider gate did not name a Task"};
        if (*gate.task_id != cursor.current_task_id)
            return {false, false, false, {}, gate.task_id,
                    "autonomous provider gate named a Task outside the pulse cursor"};

        const auto execution = executor_.execute(
            *gate.task_id, "autonomous-pulse-provider", cognition_handler_);
        if (execution != ExecuteResult::completed) {
            return {false, false, false, {}, gate.task_id,
                    "autonomous provider Task execution did not complete"};
        }

        const auto stored = task_store_.find(*gate.task_id);
        if (!stored || !gaudere::work::is_terminal(stored->status)) {
            return {false, false, true, {}, gate.task_id,
                    "autonomous provider execution left Task non-terminal"};
        }

        // Settlement remains pulse-owned. A canonical success advances generation
        // and re-arms time; a durable cognition failure is converted by the pulse
        // into its existing blocked state. Neither path may invoke provider twice.
        auto settled = pulse_service_.step();
        auto result = from_pulse_step(settled);
        result.provider_executed = true;
        result.task_id = gate.task_id;
        if (result.detail.empty())
            result.detail = "autonomous provider Task completed and pulse was re-observed";
        return result;
    }
    case GateResult::waiting:
        if (gate.retry_at) {
            auto retry = *gate.retry_at;
            const auto now = now_();
            if (retry < now) retry = now;
            static_cast<void>(scheduler_.request_at(retry));
            return {true, true, false, retry, {}, gate.detail};
        }
        return {true, false, false, {}, {}, gate.detail};
    case GateResult::dormant:
        return {true, false, false, {}, {}, gate.detail};
    case GateResult::blocked:
        // A durable safety block is not a process-health failure. Keep the host
        // service alive, but remove autonomous provider monitoring so systemd does
        // not turn a fail-closed condition into a hot restart loop.
        return {true, false, false, {}, {}, gate.detail};
    case GateResult::unavailable:
        return {true, false, false, {}, {}, gate.detail};
    }
    return {false, false, false, {}, {},
            "unknown autonomous provider gate result"};
}

} // namespace gaudere_agent
