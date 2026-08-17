#include "TaskExecutor.hpp"

#include <exception>
#include <utility>

namespace gaudere_agent {
namespace {

bool cancellation_requested(gaudere::work::TaskStore& store, const std::string& id)
{
    const auto task = store.find(id);
    return task && task->status == gaudere::work::TaskStatus::cancel_requested;
}

} // namespace

TaskExecutor::TaskExecutor(gaudere::work::Runtime& runtime,
                           gaudere::work::TaskStore& store)
    : runtime_(runtime), store_(store)
{
}

ExecuteResult TaskExecutor::execute(const std::string& id,
                                    std::string worker,
                                    TaskHandler& handler)
{
    if (!runtime_.start(id, std::move(worker))) {
        return ExecuteResult::not_startable;
    }

    const auto task = store_.find(id);
    if (!task) {
        return ExecuteResult::state_conflict;
    }

    const TaskContext context{
        *task,
        [this, id] { return cancellation_requested(store_, id); }};

    HandlerResult result;
    try {
        result = handler.execute(context);
    } catch (const std::exception& error) {
        if (!runtime_.require_manual_review(
                id, "handler_exception", error.what())) {
            return ExecuteResult::state_conflict;
        }
        return ExecuteResult::completed;
    } catch (...) {
        if (!runtime_.require_manual_review(
                id, "handler_exception", "handler threw a non-standard exception")) {
            return ExecuteResult::state_conflict;
        }
        return ExecuteResult::completed;
    }

    switch (result.outcome) {
    case HandlerOutcome::succeeded:
        if (result.content_type.empty()) {
            return runtime_.require_manual_review(
                       id, "invalid_handler_result",
                       "successful handler result has no content type")
                ? ExecuteResult::completed
                : ExecuteResult::state_conflict;
        }
        return runtime_.succeed(id, std::move(result.output),
                                std::move(result.content_type))
                       == gaudere::work::FinishResult::unavailable
            ? ExecuteResult::state_conflict
            : ExecuteResult::completed;

    case HandlerOutcome::failed:
        if (result.failure_code.empty()) {
            result.failure_code = "handler_failed";
        }
        return runtime_.fail(id, std::move(result.failure_code),
                             std::move(result.failure_message))
            ? ExecuteResult::completed
            : ExecuteResult::state_conflict;

    case HandlerOutcome::cancelled:
        if (runtime_.mark_cancelled(id)) {
            return ExecuteResult::completed;
        }
        return runtime_.require_manual_review(
                   id, "invalid_handler_result",
                   "handler returned cancelled without a durable cancellation request")
            ? ExecuteResult::completed
            : ExecuteResult::state_conflict;

    case HandlerOutcome::manual_review:
        if (result.failure_code.empty()) {
            result.failure_code = "external_effect_unknown";
        }
        return runtime_.require_manual_review(
                   id, std::move(result.failure_code),
                   std::move(result.failure_message))
            ? ExecuteResult::completed
            : ExecuteResult::state_conflict;
    }

    return ExecuteResult::state_conflict;
}

} // namespace gaudere_agent
