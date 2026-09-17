#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_SCHEDULER_BRIDGE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_SCHEDULER_BRIDGE_HPP

#include "LocalGooseCycleStore.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <optional>
#include <string>

namespace gaudere_agent {

struct LocalGooseCycleDeadlineInspection {
    bool eligible = false;
    bool active = false;
    std::optional<gaudere::scheduling::wake::Scheduler::TimePoint> deadline;
    std::string detail;
};

[[nodiscard]] LocalGooseCycleDeadlineInspection
inspect_local_goose_cycle_deadline(
    const std::optional<LocalGooseCycleCursor>& cursor) noexcept;

enum class LocalGooseCycleSchedulerArmResult {
    inactive,
    scheduled,
    advanced,
    unchanged,
    invalid
};

/** Thread-free projection of durable cycle state onto the shared Scheduler. */
class LocalGooseCycleSchedulerBridge {
public:
    explicit LocalGooseCycleSchedulerBridge(
        gaudere::scheduling::wake::Scheduler& scheduler) noexcept;

    [[nodiscard]] LocalGooseCycleSchedulerArmResult arm(
        const std::optional<LocalGooseCycleCursor>& cursor) noexcept;

private:
    gaudere::scheduling::wake::Scheduler& scheduler_;
};

} // namespace gaudere_agent

#endif
