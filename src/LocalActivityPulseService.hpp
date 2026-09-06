#ifndef GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_SERVICE_HPP
#define GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_SERVICE_HPP

#include "LocalActivityPulse.hpp"
#include "LocalActivityPulseSchedulerBridge.hpp"
#include "LocalActivityPulseStore.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <string>

namespace gaudere_agent {

struct LocalActivityPulseServiceStep {
    bool healthy = false;
    bool monitoring = false;
    LocalActivityPulseObservation observation;
    LocalActivityPulseSchedulerArmResult scheduler =
        LocalActivityPulseSchedulerArmResult::inactive;
    std::string detail;
};

/**
 * Provider-free service bridge for the bounded local continuity pulse.
 *
 * The service owns no thread and no external authority. One step asks the pulse to
 * recover/advance at most one durable generation, then derives the next exact
 * deadline from the resulting cursor and arms the existing shared Scheduler.
 * Terminal safety states stop monitoring instead of creating a retry poll loop.
 */
class LocalActivityPulseService {
public:
    LocalActivityPulseService(LocalActivityPulse& pulse,
                              LocalActivityPulseStore& store,
                              gaudere::scheduling::wake::Scheduler& scheduler) noexcept;

    [[nodiscard]] LocalActivityPulseServiceStep step();

private:
    LocalActivityPulse& pulse_;
    LocalActivityPulseStore& store_;
    LocalActivityPulseSchedulerBridge bridge_;
};

} // namespace gaudere_agent

#endif
