#ifndef GAUDERE_AGENT_TASK_DISPATCHER_HPP
#define GAUDERE_AGENT_TASK_DISPATCHER_HPP

#include "TaskExecutor.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <map>
#include <string>
#include <vector>

namespace gaudere_agent {

enum class DispatchResult {
    idle,
    dispatched,
    state_conflict
};

/** Single-owner, one-worker pending task dispatcher.
 *
 * Registered handlers are borrowed and must outlive the dispatcher. dispatch_one()
 * selects only task kinds that currently have a registered handler, then delegates
 * lifecycle changes and execution to TaskExecutor.
 */
class TaskDispatcher {
public:
    TaskDispatcher(gaudere::work::TaskStore& store, TaskExecutor& executor);

    [[nodiscard]] bool register_handler(std::string kind, TaskHandler& handler);
    [[nodiscard]] DispatchResult dispatch_one(
        const std::string& worker,
        TaskExecutor::CancellationProbe worker_stop_requested = {});

private:
    [[nodiscard]] std::vector<std::string> accepted_kinds() const;

    gaudere::work::TaskStore& store_;
    TaskExecutor& executor_;
    std::map<std::string, TaskHandler*> handlers_;
};

} // namespace gaudere_agent

#endif
