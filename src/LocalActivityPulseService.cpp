#include "LocalActivityPulseService.hpp"

#include <optional>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

bool terminal_safety_result(const LocalActivityPulseResult result) noexcept
{
    switch (result) {
    case LocalActivityPulseResult::disabled:
    case LocalActivityPulseResult::unseeded:
    case LocalActivityPulseResult::ineligible:
    case LocalActivityPulseResult::clock_rollback:
    case LocalActivityPulseResult::blocked:
    case LocalActivityPulseResult::conflict:
    case LocalActivityPulseResult::unavailable:
        return true;
    case LocalActivityPulseResult::seeded:
    case LocalActivityPulseResult::duplicate:
    case LocalActivityPulseResult::not_due:
    case LocalActivityPulseResult::preparing:
    case LocalActivityPulseResult::waiting:
    case LocalActivityPulseResult::settled:
    case LocalActivityPulseResult::quiescent:
        return false;
    }
    return true;
}

} // namespace

LocalActivityPulseService::LocalActivityPulseService(
    LocalActivityPulse& pulse,
    LocalActivityPulseStore& store,
    gaudere::scheduling::wake::Scheduler& scheduler) noexcept
    : pulse_(pulse), store_(store), bridge_(scheduler)
{
}

LocalActivityPulseServiceStep LocalActivityPulseService::step()
{
    LocalActivityPulseServiceStep step;
    try {
        step.observation = pulse_.observe();
        step.detail = step.observation.detail;

        if (terminal_safety_result(step.observation.result)) {
            step.healthy = false;
            step.monitoring = false;
            return step;
        }

        std::optional<LocalActivityPulseCursor> cursor = step.observation.cursor;
        if (!cursor) cursor = store_.find(local_activity_pulse_scope);
        if (!cursor) {
            step.detail = "local activity service lost its seeded cursor";
            return step;
        }

        step.scheduler = bridge_.arm(cursor, true);
        if (step.scheduler == LocalActivityPulseSchedulerArmResult::invalid) {
            step.detail = "local activity service could not derive a valid deadline";
            return step;
        }

        if (step.observation.result == LocalActivityPulseResult::quiescent) {
            if (step.scheduler != LocalActivityPulseSchedulerArmResult::inactive) {
                step.detail = "quiescent local activity unexpectedly retained a deadline";
                return step;
            }
            step.healthy = true;
            step.monitoring = false;
            return step;
        }

        if (step.scheduler == LocalActivityPulseSchedulerArmResult::inactive) {
            step.detail = "active local activity state has no Scheduler deadline";
            return step;
        }

        step.healthy = true;
        step.monitoring = true;
        return step;
    } catch (const std::exception& error) {
        step.detail = error.what();
        return step;
    } catch (...) {
        step.detail = "local activity service step failed";
        return step;
    }
}

} // namespace gaudere_agent
