#include "LocalGooseCognitionService.hpp"

#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/work/Task.hpp>

#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using ServiceResult = LocalGooseCognitionServiceResult;
using Step = LocalGooseCognitionServiceStep;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

bool same_definition(const Task& a, const Task& b) noexcept
{
    return a.id == b.id
        && a.idempotency_key == b.idempotency_key
        && a.kind == b.kind
        && a.input_content_type == b.input_content_type
        && a.input == b.input
        && a.limits.max_input_bytes == b.limits.max_input_bytes
        && a.limits.max_output_bytes == b.limits.max_output_bytes
        && a.limits.max_runtime == b.limits.max_runtime
        && a.limits.max_attempts == b.limits.max_attempts;
}

Step completed_step(const Task& task, const bool monitoring)
{
    if (task.status == TaskStatus::succeeded) {
        if (!canonical_local_goose_cognition_success(task) || !task.result) {
            return {false, false, ServiceResult::conflict, task, {},
                    "succeeded local Goose Task is not canonical"};
        }
        const auto inspected = inspect_local_goose_decision(task.result->output);
        if (!inspected.eligible) {
            return {false, false, ServiceResult::conflict, task, {},
                    "succeeded local Goose decision is not canonical"};
        }
        return {true, monitoring, ServiceResult::succeeded, task,
                inspected.decision, {}};
    }

    if (task.status == TaskStatus::failed
        || task.status == TaskStatus::cancelled
        || task.status == TaskStatus::manual_review) {
        // Local cognition failure is observable evidence, not authority to stop
        // the independent continuity pulse or future generations.
        return {true, monitoring, ServiceResult::failed, task, {},
                "local Goose cognition reached a terminal non-success state"};
    }
    return {true, monitoring, ServiceResult::waiting, task, {}, {}};
}

} // namespace

LocalGooseCognitionService::LocalGooseCognitionService(
    CursorReader cursor_reader,
    gaudere::work::TaskStore& task_store,
    gaudere::work::Runtime& work_runtime,
    LocalGooseCognitionHandler& handler,
    std::string model_sha256)
    : cursor_reader_(std::move(cursor_reader)), task_store_(task_store),
      work_runtime_(work_runtime), handler_(handler),
      model_sha256_(std::move(model_sha256))
{
    if (!cursor_reader_)
        throw std::invalid_argument("local Goose cursor reader is required");
}

LocalGooseCognitionServiceStep LocalGooseCognitionService::step()
{
    try {
        const auto found = cursor_reader_();
        if (!found) {
            return {false, false, ServiceResult::unavailable, {}, {},
                    "local activity pulse cursor is unavailable"};
        }
        const auto& cursor = *found;
        if (cursor.state != LocalActivityPulseState::settled
            && cursor.state != LocalActivityPulseState::quiescent) {
            const bool monitoring = cursor.state != LocalActivityPulseState::blocked
                && cursor.state != LocalActivityPulseState::quiescent;
            return {true, monitoring, ServiceResult::no_settled_observation,
                    {}, {}, {}};
        }
        if (cursor.task_id.empty() || !cursor.result_sha256) {
            return {false, false, ServiceResult::conflict, {}, {},
                    "settled local activity cursor lacks Task/result identity"};
        }

        const bool monitoring = cursor.state != LocalActivityPulseState::quiescent;
        const auto source = task_store_.find(cursor.task_id);
        if (!source || source->status != TaskStatus::succeeded
            || !canonical_local_continuity_observation_success(*source)
            || !source->result
            || sha256_hex(source->result->output) != *cursor.result_sha256) {
            return {false, false, ServiceResult::conflict, source, {},
                    "settled local observation source is missing or non-canonical"};
        }

        const auto expected = make_local_goose_cognition_task(
            *source, model_sha256_);
        auto by_id = task_store_.find(expected.id);
        auto by_key = task_store_.find_by_idempotency_key(
            expected.idempotency_key);
        std::optional<Task> task;
        if (by_id || by_key) {
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, expected)) {
                return {false, false, ServiceResult::conflict,
                        by_id ? by_id : by_key, {},
                        "deterministic local Goose Task conflicts"};
            }
            task = by_id;
        } else {
            if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
                return {true, monitoring, ServiceResult::unavailable, {}, {},
                        "work runtime is not running for local Goose submission"};
            }
            const auto submitted = work_runtime_.submit(expected);
            if (submitted == gaudere::work::SubmitResult::invalid) {
                return {false, false, ServiceResult::conflict, {}, {},
                        "work runtime rejected canonical local Goose Task"};
            }
            if (submitted == gaudere::work::SubmitResult::unavailable) {
                return {true, monitoring, ServiceResult::unavailable, {}, {},
                        "work runtime could not submit local Goose Task"};
            }
            by_id = task_store_.find(expected.id);
            by_key = task_store_.find_by_idempotency_key(
                expected.idempotency_key);
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, expected)) {
                return {false, false, ServiceResult::conflict,
                        by_id ? by_id : by_key, {},
                        "submitted local Goose Task did not persist exactly"};
            }
            task = by_id;
        }

        if (!task) {
            return {true, monitoring, ServiceResult::unavailable, {}, {},
                    "local Goose Task lookup returned no Task"};
        }
        if (gaudere::work::is_terminal(task->status))
            return completed_step(*task, monitoring);
        if (task->status != TaskStatus::pending) {
            return {true, monitoring, ServiceResult::waiting, task, {},
                    "local Goose Task is already active or cancellation-pending"};
        }
        if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
            return {true, monitoring, ServiceResult::unavailable, task, {},
                    "work runtime is not running for local Goose execution"};
        }

        TaskExecutor executor(work_runtime_, task_store_);
        const auto executed = executor.execute(
            task->id, "local-goose-cognition", handler_);
        if (executed == ExecuteResult::state_conflict) {
            return {false, false, ServiceResult::conflict, task, {},
                    "local Goose execution encountered state conflict"};
        }
        if (executed == ExecuteResult::not_startable) {
            return {true, monitoring, ServiceResult::waiting, task, {},
                    "local Goose Task is not startable yet"};
        }

        task = task_store_.find(expected.id);
        if (!task) {
            return {false, false, ServiceResult::conflict, {}, {},
                    "executed local Goose Task disappeared"};
        }
        return completed_step(*task, monitoring);
    } catch (const std::exception& error) {
        return {true, true, ServiceResult::unavailable, {}, {}, error.what()};
    }
}

} // namespace gaudere_agent
