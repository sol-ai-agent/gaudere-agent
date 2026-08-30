#include "AutonomousCognitionProviderService.hpp"

#include "OpenAIBudget.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using TimePoint = gaudere::scheduling::wake::Scheduler::TimePoint;

std::optional<TimePoint> provider_window_retry(
    const TimePoint now) noexcept
{
    const auto policy = openai_bootstrap_budget_policy();
    const auto delay = std::chrono::duration_cast<TimePoint::duration>(policy.window);
    if (delay.count() < 0 || delay > TimePoint::max() - now) return std::nullopt;
    return now + delay;
}

} // namespace

AutonomousCognitionProviderService::AutonomousCognitionProviderService(
    AutonomousCognitionProviderGate& gate,
    TaskExecutor& executor,
    TaskHandler& handler,
    AutonomousCognitionPulseService& pulse_service,
    gaudere::scheduling::wake::Scheduler& scheduler,
    Now now)
    : gate_(gate), executor_(executor), handler_(handler),
      pulse_service_(pulse_service), scheduler_(scheduler), now_(std::move(now))
{
    if (!now_) {
        throw std::invalid_argument(
            "autonomous cognition provider service clock is required");
    }
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

    case GateResult::waiting: {
        result.detail = result.gate.detail;
        auto retry = result.gate.retry_at;
        if (!retry
            && result.gate.detail == "provider rolling-window budget is exhausted") {
            retry = provider_window_retry(now_());
            if (!retry) {
                result.healthy = false;
                result.detail = "provider rolling-window retry deadline overflows";
                return result;
            }
        }
        if (retry) {
            static_cast<void>(scheduler_.request_at(*retry));
            result.monitoring = true;
            result.next_at = retry;
        }
        return result;
    }

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
