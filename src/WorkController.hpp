#ifndef GAUDERE_AGENT_WORK_CONTROLLER_HPP
#define GAUDERE_AGENT_WORK_CONTROLLER_HPP

#include "TaskDispatcher.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <cstddef>
#include <string>

namespace gaudere_agent {

enum class WorkCycleResult {
    idle,
    worked,
    state_conflict,
    stopped
};

/** Event-driven single-worker controller for bounded work.
 *
 * The controller owns no thread and performs no polling. start() requests an
 * immediate first wake. notify_work() advances the scheduler to an immediate
 * wake after accepted in-process work. wait_and_run() blocks in Scheduler,
 * recovers expired leases, drains eligible pending work through TaskDispatcher,
 * then schedules the exact next lease recovery deadline.
 *
 * stop() is safe to call from another thread: it only publishes the stop request
 * and wakes/stops Scheduler. The worker thread that is inside wait_and_run()
 * performs the Runtime transition to draining, so runtime state changes never
 * race a synchronous TaskHandler invocation.
 */
class WorkController {
public:
    WorkController(gaudere::scheduling::wake::Scheduler& scheduler,
                   gaudere::work::Runtime& runtime,
                   TaskDispatcher& dispatcher,
                   std::string worker);

    [[nodiscard]] bool start();
    void notify_work();
    [[nodiscard]] WorkCycleResult wait_and_run();
    void stop();

private:
    [[nodiscard]] WorkCycleResult enter_draining();
    void schedule_recovery_deadline();

    gaudere::scheduling::wake::Scheduler& scheduler_;
    gaudere::work::Runtime& runtime_;
    TaskDispatcher& dispatcher_;
    std::string worker_;
    std::atomic_bool started_{false};
    std::atomic_bool stopping_{false};
};

} // namespace gaudere_agent

#endif
