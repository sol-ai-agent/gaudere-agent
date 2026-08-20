#ifndef GAUDERE_AGENT_TASK_EXECUTOR_HPP
#define GAUDERE_AGENT_TASK_EXECUTOR_HPP

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <string>
#include <utility>

namespace gaudere_agent {

enum class HandlerOutcome {
    succeeded,
    failed,
    cancelled,
    manual_review
};

struct HandlerResult {
    HandlerResult() = default;

    HandlerResult(HandlerOutcome outcome_value,
                  std::string content_type_value,
                  std::string output_value,
                  std::string failure_code_value,
                  std::string failure_message_value,
                  std::string metadata_content_type_value = {},
                  std::string metadata_value = {})
        : outcome(outcome_value),
          content_type(std::move(content_type_value)),
          output(std::move(output_value)),
          failure_code(std::move(failure_code_value)),
          failure_message(std::move(failure_message_value)),
          metadata_content_type(std::move(metadata_content_type_value)),
          metadata(std::move(metadata_value))
    {
    }

    HandlerOutcome outcome = HandlerOutcome::failed;
    std::string content_type;
    std::string output;
    std::string failure_code;
    std::string failure_message;
    std::string metadata_content_type;
    std::string metadata;
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
    using CancellationProbe = std::function<bool()>;

    TaskExecutor(gaudere::work::Runtime& runtime, gaudere::work::TaskStore& store);

    [[nodiscard]] ExecuteResult execute(const std::string& id,
                                        std::string worker,
                                        TaskHandler& handler,
                                        CancellationProbe worker_stop_requested = {});

private:
    gaudere::work::Runtime& runtime_;
    gaudere::work::TaskStore& store_;
};

} // namespace gaudere_agent

#endif
