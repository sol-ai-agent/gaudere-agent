#ifndef GAUDERE_AGENT_TASK_EXECUTOR_HPP
#define GAUDERE_AGENT_TASK_EXECUTOR_HPP

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <string>

namespace gaudere_agent {

enum class HandlerOutcome {
    succeeded,
    failed,
    cancelled,
    manual_review
};

struct HandlerResult {
    HandlerOutcome outcome = HandlerOutcome::failed;
    std::string content_type;
    std::string output;
    std::string failure_code;
    std::string failure_message;
};

struct TaskContext {
    const gaudere::work::Task& task;
    std::function<bool()> cancellation_requested;
};

class TaskHandler {
public:
    virtual ~TaskHandler() = default;
    [[nodiscard]] virtual HandlerResult execute(const TaskContext& context) = 0;
};

enum class ExecuteResult {
    completed,
    not_startable,
    state_conflict
};

class TaskExecutor {
public:
    TaskExecutor(gaudere::work::Runtime& runtime, gaudere::work::TaskStore& store);

    [[nodiscard]] ExecuteResult execute(const std::string& id,
                                        std::string worker,
                                        TaskHandler& handler);

private:
    gaudere::work::Runtime& runtime_;
    gaudere::work::TaskStore& store_;
};

} // namespace gaudere_agent

#endif
