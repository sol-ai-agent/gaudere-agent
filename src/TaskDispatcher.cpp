#include "TaskDispatcher.hpp"

#include <utility>

namespace gaudere_agent {

TaskDispatcher::TaskDispatcher(gaudere::work::TaskStore& store,
                               TaskExecutor& executor)
    : store_(store), executor_(executor)
{
}

bool TaskDispatcher::register_handler(std::string kind, TaskHandler& handler)
{
    if (kind.empty() || handlers_.find(kind) != handlers_.end()) {
        return false;
    }
    handlers_.emplace(std::move(kind), &handler);
    return true;
}

std::vector<std::string> TaskDispatcher::accepted_kinds() const
{
    std::vector<std::string> kinds;
    kinds.reserve(handlers_.size());
    for (const auto& entry : handlers_) {
        kinds.push_back(entry.first);
    }
    return kinds;
}

DispatchResult TaskDispatcher::dispatch_one(
    const std::string& worker,
    TaskExecutor::CancellationProbe worker_stop_requested)
{
    if (worker.empty()) {
        return DispatchResult::state_conflict;
    }

    const auto task = store_.find_pending_for(accepted_kinds());
    if (!task) {
        return DispatchResult::idle;
    }

    const auto found = handlers_.find(task->kind);
    if (found == handlers_.end() || found->second == nullptr) {
        return DispatchResult::state_conflict;
    }

    switch (executor_.execute(task->id, worker, *found->second,
                              std::move(worker_stop_requested))) {
    case ExecuteResult::completed:
        return DispatchResult::dispatched;
    case ExecuteResult::not_startable:
    case ExecuteResult::state_conflict:
        return DispatchResult::state_conflict;
    }

    return DispatchResult::state_conflict;
}

} // namespace gaudere_agent
