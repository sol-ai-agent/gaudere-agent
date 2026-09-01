#include "AutonomousCognitionPulseService.hpp"

#include "OpenAIBudget.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using PulseResult = AutonomousCognitionPulseResult;
using PulseState = AutonomousCognitionPulseState;
using RefreshResult = AutonomousCognitionStaleRefreshResult;
using TimePoint = gaudere::scheduling::wake::Scheduler::TimePoint;

TimePoint from_milliseconds(const std::int64_t value)
{
    return TimePoint{std::chrono::milliseconds{value}};
}

std::optional<TimePoint> add_checked(
    const TimePoint base,
    const std::chrono::milliseconds delay) noexcept
{
    if (delay.count() < 0 || delay > TimePoint::max() - base) return std::nullopt;
    return base + delay;
}

AutonomousCognitionPulseServicePlan cursor_plan(
    const AutonomousCognitionPulseObservation& observation,
    const TimePoint now) noexcept
{
    if (!observation.cursor) {
        return {false, false, {}, "pulse observation has no durable cursor"};
    }

    switch (observation.cursor->state) {
    case PulseState::idle:
    case PulseState::quiescent: {
        auto deadline = from_milliseconds(observation.cursor->due_at_ms);
        if (deadline < now) deadline = now;
        return {true, true, deadline, {}};
    }
    case PulseState::preparing:
        return {true, true, now, {}};
    case PulseState::prepared:
        return {true, false, {},
                "prepared cognition awaits separate provider authority"};
    case PulseState::blocked:
        return {false, false, {}, observation.cursor->blocked_reason};
    }
    return {false, false, {}, "unknown autonomous pulse cursor state"};
}

AutonomousCognitionPulseServicePlan budget_retry_plan(
    const std::optional<gaudere::budget::Snapshot>& budget,
    const TimePoint now,
    const gaudere::budget::Policy& policy) noexcept
{
    if (!budget) {
        return {false, false, {},
                "budget-blocked pulse has no read-only budget snapshot"};
    }

    using Result = gaudere::budget::ConsumeResult;
    switch (budget->next_new_consumption) {
    case Result::accepted:
        return {true, true, now,
                "provider budget became eligible after pulse observation"};
    case Result::cooldown: {
        if (!budget->last_consumed_at) {
            return {false, false, {},
                    "provider cooldown has no durable last-consumed timestamp"};
        }
        const auto deadline = add_checked(*budget->last_consumed_at,
                                          policy.min_interval);
        if (!deadline) {
            return {false, false, {}, "provider cooldown deadline overflows"};
        }
        return {true, true, *deadline < now ? now : *deadline,
                "provider cooldown defers autonomous pulse"};
    }
    case Result::window_exhausted: {
        const auto deadline = add_checked(now, policy.window);
        if (!deadline) {
            return {false, false, {}, "provider window retry deadline overflows"};
        }
        return {true, true, deadline,
                "provider rolling window defers autonomous pulse"};
    }
    case Result::total_exhausted:
        return {true, false, {},
                "provider lifetime budget exhausted; autonomous pulse dormant"};
    case Result::clock_rollback:
        return {false, false, {},
                "provider budget clock rollback blocks autonomous pulse"};
    case Result::duplicate:
        return {false, false, {},
                "read-only provider budget snapshot returned impossible duplicate"};
    }
    return {false, false, {}, "unknown provider budget state"};
}

} // namespace

AutonomousCognitionPulseServicePlan plan_autonomous_cognition_pulse_service(
    const AutonomousCognitionPulseObservation& observation,
    const std::optional<gaudere::budget::Snapshot>& budget,
    const TimePoint now,
    const gaudere::budget::Policy& policy) noexcept
{
    if (!gaudere::budget::valid_policy(policy)) {
        return {false, false, {}, "autonomous pulse budget policy is invalid"};
    }

    switch (observation.result) {
    case PulseResult::seeded:
    case PulseResult::duplicate:
    case PulseResult::not_due:
    case PulseResult::settled_continue:
    case PulseResult::settled_stop:
        return cursor_plan(observation, now);
    case PulseResult::preparing:
        return {true, true, now, "autonomous pulse preparation should resume"};
    case PulseResult::budget_blocked:
        return budget_retry_plan(budget, now, policy);
    case PulseResult::prepared:
    case PulseResult::waiting:
        return {true, false, {},
                "prepared cognition awaits separate provider authority"};
    case PulseResult::disabled:
        return {false, false, {}, "autonomous cognition pulse is disabled"};
    case PulseResult::unseeded:
        return {false, false, {}, "autonomous cognition pulse is unseeded"};
    case PulseResult::clock_rollback:
        return {false, false, {}, "autonomous cognition pulse clock rollback"};
    case PulseResult::blocked:
        return cursor_plan(observation, now);
    case PulseResult::conflict:
        return {false, false, {}, "autonomous cognition pulse conflict"};
    case PulseResult::unavailable:
        return {false, false, {}, "autonomous cognition pulse unavailable"};
    }
    return {false, false, {}, "unknown autonomous cognition pulse result"};
}

AutonomousCognitionPulseService::AutonomousCognitionPulseService(
    AutonomousCognitionPulse& pulse,
    gaudere::budget::Store& budget_store,
    gaudere::scheduling::wake::Scheduler& scheduler,
    Now now,
    AutonomousCognitionStaleRefresh* stale_refresh)
    : pulse_(pulse), budget_store_(budget_store), scheduler_(scheduler),
      now_(std::move(now)), stale_refresh_(stale_refresh)
{
    if (!now_) throw std::invalid_argument("autonomous pulse service clock is required");
}

AutonomousCognitionPulseServiceStep AutonomousCognitionPulseService::step()
{
    bool retired_stale = false;
    std::string refresh_detail;
    if (stale_refresh_) {
        const auto refresh = stale_refresh_->step();
        switch (refresh.result) {
        case RefreshResult::retired:
            retired_stale = true;
            refresh_detail = refresh.detail;
            break;
        case RefreshResult::blocked: {
            auto observation = pulse_.observe();
            return {std::move(observation),
                    {true, false, {}, refresh.detail.empty()
                        ? "stale cognition refresh blocked"
                        : refresh.detail}};
        }
        case RefreshResult::unavailable: {
            auto observation = pulse_.observe();
            return {std::move(observation),
                    {true, false, {}, refresh.detail.empty()
                        ? "stale cognition refresh unavailable"
                        : refresh.detail}};
        }
        case RefreshResult::not_applicable:
            break;
        }
    }

    const auto now = now_();
    auto observation = pulse_.observe();
    std::optional<gaudere::budget::Snapshot> budget;
    const auto policy = openai_bootstrap_budget_policy();
    if (observation.result == AutonomousCognitionPulseResult::budget_blocked) {
        budget = budget_store_.snapshot(
            std::string{openai_budget_scope()}, now, policy);
    }
    auto plan = plan_autonomous_cognition_pulse_service(
        observation, budget, now, policy);
    if (retired_stale && plan.detail.empty()) {
        plan.detail = refresh_detail.empty()
            ? "stale unspent cognition retired before fresh pulse observation"
            : refresh_detail;
    }
    if (plan.monitoring && plan.next_at) {
        static_cast<void>(scheduler_.request_at(*plan.next_at));
    }
    return {std::move(observation), std::move(plan)};
}

} // namespace gaudere_agent
