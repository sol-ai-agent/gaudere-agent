#include "LocalActivityPulseSchedulerBridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace gaudere_agent {
namespace {

using Scheduler = gaudere::scheduling::wake::Scheduler;

std::optional<Scheduler::TimePoint> deadline_from_ms(const std::int64_t value) noexcept
{
    if (value < 0) return std::nullopt;
    return Scheduler::TimePoint{std::chrono::milliseconds{value}};
}

bool add_cadence(const std::int64_t base, std::int64_t& result) noexcept
{
    if (base < 0
        || base > std::numeric_limits<std::int64_t>::max()
            - local_activity_pulse_cadence_ms) {
        return false;
    }
    result = base + local_activity_pulse_cadence_ms;
    return true;
}

} // namespace

LocalActivityPulseDeadlineInspection inspect_local_activity_pulse_deadline(
    const std::optional<LocalActivityPulseCursor>& cursor,
    const bool enabled) noexcept
{
    LocalActivityPulseDeadlineInspection inspection;
    try {
        if (!enabled || !cursor) {
            inspection.eligible = true;
            return inspection;
        }
        if (!valid_local_activity_pulse_cursor(*cursor)) {
            inspection.detail = "local activity pulse cursor is invalid";
            return inspection;
        }

        std::int64_t deadline_ms = 0;
        switch (cursor->state) {
        case LocalActivityPulseState::blocked:
        case LocalActivityPulseState::quiescent:
            inspection.eligible = true;
            return inspection;

        case LocalActivityPulseState::idle:
            deadline_ms = cursor->due_at_ms;
            break;

        case LocalActivityPulseState::preparing:
            if (!cursor->captured_at_ms) {
                inspection.detail = "preparing local pulse cursor has no capture time";
                return inspection;
            }
            deadline_ms = std::max(cursor->due_at_ms, *cursor->captured_at_ms);
            break;

        case LocalActivityPulseState::settled:
            if (!cursor->captured_at_ms || !cursor->result_sha256
                || cursor->task_id.empty() || cursor->generation >= 3) {
                inspection.detail = "settled local pulse cursor is incomplete";
                return inspection;
            }
            if (!add_cadence(*cursor->captured_at_ms, deadline_ms)) {
                inspection.detail = "next local pulse deadline overflows";
                return inspection;
            }
            break;
        }

        inspection.deadline = deadline_from_ms(deadline_ms);
        if (!inspection.deadline) {
            inspection.detail = "local pulse deadline is outside supported time range";
            return inspection;
        }
        inspection.active = true;
        inspection.eligible = true;
        return inspection;
    } catch (...) {
        inspection.detail = "local activity pulse deadline inspection failed";
        return inspection;
    }
}

LocalActivityPulseSchedulerBridge::LocalActivityPulseSchedulerBridge(
    gaudere::scheduling::wake::Scheduler& scheduler) noexcept
    : scheduler_(scheduler)
{
}

LocalActivityPulseSchedulerArmResult LocalActivityPulseSchedulerBridge::arm(
    const std::optional<LocalActivityPulseCursor>& cursor,
    const bool enabled) noexcept
{
    const auto inspection = inspect_local_activity_pulse_deadline(cursor, enabled);
    if (!inspection.eligible) return LocalActivityPulseSchedulerArmResult::invalid;
    if (!inspection.active || !inspection.deadline)
        return LocalActivityPulseSchedulerArmResult::inactive;

    switch (scheduler_.request_at(*inspection.deadline)) {
    case gaudere::scheduling::wake::Update::scheduled:
        return LocalActivityPulseSchedulerArmResult::scheduled;
    case gaudere::scheduling::wake::Update::advanced:
        return LocalActivityPulseSchedulerArmResult::advanced;
    case gaudere::scheduling::wake::Update::unchanged:
        return LocalActivityPulseSchedulerArmResult::unchanged;
    }
    return LocalActivityPulseSchedulerArmResult::invalid;
}

} // namespace gaudere_agent
