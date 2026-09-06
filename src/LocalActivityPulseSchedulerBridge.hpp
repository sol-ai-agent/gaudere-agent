#ifndef GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_SCHEDULER_BRIDGE_HPP
#define GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_SCHEDULER_BRIDGE_HPP

#include "LocalActivityPulseStore.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <optional>
#include <string>

namespace gaudere_agent {

struct LocalActivityPulseDeadlineInspection {
    bool eligible = false;
    bool active = false;
    std::optional<gaudere::scheduling::wake::Scheduler::TimePoint> deadline;
    std::string detail;
};

/**
 * Pure read-only derivation of the one exact deadline implied by durable pulse state.
 *
 * disabled/unseeded/blocked/quiescent states are eligible but inactive. `preparing`
 * uses max(due,captured) so a backward wall clock cannot create a busy immediate
 * recovery loop before the frozen capture time is reached again.
 */
[[nodiscard]] LocalActivityPulseDeadlineInspection
inspect_local_activity_pulse_deadline(
    const std::optional<LocalActivityPulseCursor>& cursor,
    bool enabled) noexcept;

enum class LocalActivityPulseSchedulerArmResult {
    inactive,
    scheduled,
    advanced,
    unchanged,
    invalid
};

/**
 * Thread-free bridge from durable local-pulse state to the existing one-deadline
 * Scheduler. It owns no thread and never creates or mutates WakeIntent state.
 *
 * The normal loop is: arm -> Scheduler::wait() consumes due -> pulse.observe() ->
 * arm again. An interrupted wait keeps the same Scheduler deadline, so re-arming the
 * same durable cursor is idempotent. Inactive state intentionally does not call
 * Scheduler::stop(), because stop is permanent; callers reach inactive state after a
 * consumed deadline or before any deadline is armed.
 */
class LocalActivityPulseSchedulerBridge {
public:
    explicit LocalActivityPulseSchedulerBridge(
        gaudere::scheduling::wake::Scheduler& scheduler) noexcept;

    [[nodiscard]] LocalActivityPulseSchedulerArmResult arm(
        const std::optional<LocalActivityPulseCursor>& cursor,
        bool enabled) noexcept;

private:
    gaudere::scheduling::wake::Scheduler& scheduler_;
};

} // namespace gaudere_agent

#endif
