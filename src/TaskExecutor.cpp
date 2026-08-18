#include "TaskExecutor.hpp"

#include <exception>
#include <utility>

namespace gaudere_agent {
namespace {

bool durable_cancellation_requested(gaudere::work::TaskStore& store,
                                    const std::string& id)
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
                                    TaskHandler& handler,
                                    CancellationProbe worker_stop_requested)
{
    if (!runtime_.start(id, std::move(worker))) {
        return ExecuteResult::not_startable;
    }

    const auto task = store_.find(id);
    if (!task) {
        return ExecuteResult::state_conflict;
    }

    const auto cancellation_probe = [this, id, worker_stop_requested] {
        return durable_cancellation_requested(store_, id)
            || (worker_stop_requested && worker_stop_requested());
    };
    const TaskContext context{*task, cancellation_probe};

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
        if (worker_stop_requested && worker_stop_requested()) {
            if (!runtime_.request_cancel(id, "worker shutdown requested")) {
                return ExecuteResult::state_conflict;
            }
            return runtime_.mark_cancelled(id)
                ? ExecuteResult::completed
                : ExecuteResult::state_conflict;
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
