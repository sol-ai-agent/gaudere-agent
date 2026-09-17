#include "LocalGooseCycleSchedulerBridge.hpp"

#include <chrono>
#include <cstdint>

namespace gaudere_agent {
namespace {

using Scheduler = gaudere::scheduling::wake::Scheduler;

std::optional<Scheduler::TimePoint> deadline_from_ms(const std::int64_t value) noexcept
{
    if (value < 0) return std::nullopt;
    return Scheduler::TimePoint{std::chrono::milliseconds{value}};
}

} // namespace

LocalGooseCycleDeadlineInspection inspect_local_goose_cycle_deadline(
    const std::optional<LocalGooseCycleCursor>& cursor) noexcept
{
    LocalGooseCycleDeadlineInspection inspection;
    try {
        if (!cursor) {
            inspection.eligible = true;
            return inspection;
        }
        if (!valid_local_goose_cycle_cursor(*cursor)) {
            inspection.detail = "Local Goose cycle cursor is invalid";
            return inspection;
        }
        if (cursor->state != LocalGooseCycleState::scheduled) {
            inspection.eligible = true;
            return inspection;
        }
        if (!cursor->due_at_ms) {
            inspection.detail = "scheduled Local Goose cycle lacks durable deadline";
            return inspection;
        }
        inspection.deadline = deadline_from_ms(*cursor->due_at_ms);
        if (!inspection.deadline) {
            inspection.detail = "Local Goose cycle deadline is outside supported range";
            return inspection;
        }
        inspection.active = true;
        inspection.eligible = true;
        return inspection;
    } catch (...) {
        inspection.detail = "Local Goose cycle deadline inspection failed";
        return inspection;
    }
}

LocalGooseCycleSchedulerBridge::LocalGooseCycleSchedulerBridge(
    gaudere::scheduling::wake::Scheduler& scheduler) noexcept
    : scheduler_(scheduler)
{
}

LocalGooseCycleSchedulerArmResult LocalGooseCycleSchedulerBridge::arm(
    const std::optional<LocalGooseCycleCursor>& cursor) noexcept
{
    const auto inspection = inspect_local_goose_cycle_deadline(cursor);
    if (!inspection.eligible) return LocalGooseCycleSchedulerArmResult::invalid;
    if (!inspection.active || !inspection.deadline)
        return LocalGooseCycleSchedulerArmResult::inactive;

    switch (scheduler_.request_at(*inspection.deadline)) {
    case gaudere::scheduling::wake::Update::scheduled:
        return LocalGooseCycleSchedulerArmResult::scheduled;
    case gaudere::scheduling::wake::Update::advanced:
        return LocalGooseCycleSchedulerArmResult::advanced;
    case gaudere::scheduling::wake::Update::unchanged:
        return LocalGooseCycleSchedulerArmResult::unchanged;
    }
    return LocalGooseCycleSchedulerArmResult::invalid;
}

} // namespace gaudere_agent
