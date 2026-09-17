#include "LocalGooseCycleService.hpp"

#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/work/Task.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using Cursor = LocalGooseCycleCursor;
using ServiceResult = LocalGooseCycleServiceResult;
using Step = LocalGooseCycleServiceStep;
using StoreResult = LocalGooseCycleStoreResult;
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

bool add_delay(const std::int64_t base,
               const std::int64_t requested,
               std::int64_t& result) noexcept
{
    if (base < 0 || requested < 0) return false;
    const auto delay = std::max(requested, local_goose_cycle_minimum_delay_ms);
    if (base > std::numeric_limits<std::int64_t>::max() - delay) return false;
    result = base + delay;
    return true;
}

std::optional<Task> exact_cycle_task(const Cursor& cursor,
                                     gaudere::work::TaskStore& task_store,
                                     const std::string& model_sha256,
                                     std::string& detail)
{
    if (!cursor.due_at_ms || !cursor.captured_at_ms) {
        detail = "prepared Local Goose cycle lacks durable timing";
        return std::nullopt;
    }

    const auto anchor = task_store.find(cursor.anchor_observation_task_id);
    if (!anchor || !anchor->result
        || !canonical_local_continuity_observation_success(*anchor)
        || sha256_hex(anchor->result->output)
            != cursor.anchor_observation_result_sha256) {
        detail = "Local Goose cycle anchor Task/result is missing or non-canonical";
        return std::nullopt;
    }

    std::optional<Task> predecessor;
    if (cursor.generation >= 2) {
        if (!cursor.predecessor_task_id || !cursor.predecessor_result_sha256) {
            detail = "recurring Local Goose cycle lacks predecessor evidence";
            return std::nullopt;
        }
        predecessor = task_store.find(*cursor.predecessor_task_id);
        if (!predecessor || !predecessor->result
            || !canonical_local_goose_cycle_success(*predecessor)
            || sha256_hex(predecessor->result->output)
                != *cursor.predecessor_result_sha256) {
            detail = "Local Goose cycle predecessor Task/result is missing or non-canonical";
            return std::nullopt;
        }
    }

    try {
        return make_local_goose_cycle_task(
            *anchor, model_sha256, cursor.generation,
            *cursor.due_at_ms, *cursor.captured_at_ms, predecessor);
    } catch (const std::exception& error) {
        detail = error.what();
        return std::nullopt;
    }
}

Step write_blocked(LocalGooseCycleStore& store,
                   const Cursor& current,
                   const std::string& reason,
                   const std::optional<Task>& task = std::nullopt)
{
    Cursor blocked = current;
    ++blocked.revision;
    blocked.state = LocalGooseCycleState::blocked;
    blocked.blocked_reason = reason;
    const auto write = store.replace(current, blocked);
    if (write.result == StoreResult::accepted
        || write.result == StoreResult::duplicate) {
        return {true, false, ServiceResult::blocked,
                write.cursor ? write.cursor : std::optional<Cursor>{blocked},
                task, {}, reason};
    }
    if (write.result == StoreResult::unavailable) {
        return {false, true, ServiceResult::unavailable,
                write.cursor, task, {}, write.detail};
    }
    return {false, false, ServiceResult::conflict,
            write.cursor, task, {},
            write.detail.empty() ? "failed to persist blocked Local Goose cycle"
                                 : write.detail};
}

} // namespace

LocalGooseCycleService::LocalGooseCycleService(
    LocalGooseCycleStore& cycle_store,
    gaudere::work::TaskStore& task_store,
    gaudere::work::Runtime& work_runtime,
    LocalGooseCycleHandler& handler,
    std::string model_sha256,
    NowMs now_ms)
    : cycle_store_(cycle_store), task_store_(task_store),
      work_runtime_(work_runtime), handler_(handler),
      model_sha256_(std::move(model_sha256)), now_ms_(std::move(now_ms))
{
    if (!now_ms_)
        throw std::invalid_argument("Local Goose cycle clock is required");
}

LocalGooseCycleServiceStep LocalGooseCycleService::step()
{
    try {
        const auto found = cycle_store_.find(local_goose_cycle_scope);
        if (!found) {
            return {false, false, ServiceResult::unavailable, {}, {}, {},
                    "Local Goose cycle sidecar is unseeded"};
        }
        const Cursor cursor = *found;

        if (cursor.state == LocalGooseCycleState::dormant) {
            return {true, false, ServiceResult::dormant, cursor, {}, {}, {}};
        }
        if (cursor.state == LocalGooseCycleState::blocked) {
            return {true, false, ServiceResult::blocked, cursor, {}, {},
                    cursor.blocked_reason};
        }

        const auto now = now_ms_();
        if (now < 0) {
            return {false, true, ServiceResult::unavailable, cursor, {}, {},
                    "Local Goose cycle clock is negative"};
        }

        if (cursor.state == LocalGooseCycleState::scheduled) {
            if (!cursor.due_at_ms) {
                return write_blocked(cycle_store_, cursor,
                    "scheduled Local Goose cycle has no deadline");
            }
            if (now < *cursor.due_at_ms) {
                return {true, true, ServiceResult::waiting, cursor, {}, {}, {}};
            }

            Cursor prepared = cursor;
            ++prepared.revision;
            prepared.state = LocalGooseCycleState::prepared;
            prepared.captured_at_ms = now;
            prepared.blocked_reason.clear();

            std::string detail;
            const auto expected = exact_cycle_task(
                prepared, task_store_, model_sha256_, detail);
            if (!expected) {
                return write_blocked(cycle_store_, cursor,
                    detail.empty() ? "cannot derive exact Local Goose cycle Task" : detail);
            }
            prepared.current_task_id = expected->id;
            if (!valid_local_goose_cycle_transition(cursor, prepared)) {
                return write_blocked(cycle_store_, cursor,
                    "scheduled-to-prepared Local Goose cycle transition is non-canonical");
            }
            const auto write = cycle_store_.replace(cursor, prepared);
            if (write.result == StoreResult::accepted
                || write.result == StoreResult::duplicate) {
                return {true, true, ServiceResult::prepared,
                        write.cursor ? write.cursor : std::optional<Cursor>{prepared},
                        expected, {}, {}};
            }
            if (write.result == StoreResult::unavailable) {
                return {false, true, ServiceResult::unavailable,
                        write.cursor, expected, {}, write.detail};
            }
            return {false, false, ServiceResult::conflict,
                    write.cursor, expected, {},
                    write.detail.empty() ? "Local Goose cycle capture CAS conflict"
                                         : write.detail};
        }

        if (cursor.state != LocalGooseCycleState::prepared) {
            return {false, false, ServiceResult::conflict, cursor, {}, {},
                    "unknown Local Goose cycle state"};
        }

        std::string detail;
        const auto expected = exact_cycle_task(
            cursor, task_store_, model_sha256_, detail);
        if (!expected || expected->id != cursor.current_task_id) {
            return write_blocked(cycle_store_, cursor,
                detail.empty() ? "prepared Local Goose cycle Task identity differs" : detail);
        }

        auto by_id = task_store_.find(expected->id);
        auto by_key = task_store_.find_by_idempotency_key(expected->idempotency_key);
        std::optional<Task> task;
        if (by_id || by_key) {
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, *expected)) {
                return write_blocked(cycle_store_, cursor,
                    "deterministic Local Goose cycle Task conflicts",
                    by_id ? by_id : by_key);
            }
            task = by_id;
        } else {
            if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
                return {true, true, ServiceResult::unavailable,
                        cursor, {}, {}, "work runtime is not running for cycle submission"};
            }
            const auto submitted = work_runtime_.submit(*expected);
            if (submitted == gaudere::work::SubmitResult::invalid) {
                return write_blocked(cycle_store_, cursor,
                    "work runtime rejected canonical Local Goose cycle Task");
            }
            if (submitted == gaudere::work::SubmitResult::unavailable) {
                return {true, true, ServiceResult::unavailable,
                        cursor, {}, {}, "work runtime could not submit cycle Task"};
            }
            by_id = task_store_.find(expected->id);
            by_key = task_store_.find_by_idempotency_key(expected->idempotency_key);
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, *expected)) {
                return write_blocked(cycle_store_, cursor,
                    "submitted Local Goose cycle Task did not persist exactly",
                    by_id ? by_id : by_key);
            }
            return {true, true, ServiceResult::submitted,
                    cursor, by_id, {}, {}};
        }

        if (!task) {
            return {false, true, ServiceResult::unavailable,
                    cursor, {}, {}, "Local Goose cycle Task lookup returned no Task"};
        }

        if (gaudere::work::is_terminal(task->status)) {
            if (task->status != TaskStatus::succeeded) {
                return write_blocked(cycle_store_, cursor,
                    "Local Goose cycle Task reached terminal non-success state", task);
            }
            if (!canonical_local_goose_cycle_success(*task) || !task->result) {
                return write_blocked(cycle_store_, cursor,
                    "succeeded Local Goose cycle Task is non-canonical", task);
            }
            const auto inspected = inspect_local_goose_decision(task->result->output);
            if (!inspected.eligible) {
                return write_blocked(cycle_store_, cursor,
                    "succeeded Local Goose cycle decision is non-canonical", task);
            }

            Cursor replacement = cursor;
            ++replacement.revision;
            ++replacement.generation;
            replacement.predecessor_task_id = task->id;
            replacement.predecessor_result_sha256 = sha256_hex(task->result->output);
            replacement.captured_at_ms.reset();
            replacement.current_task_id.clear();
            replacement.blocked_reason.clear();

            std::optional<std::int64_t> requested_delay =
                inspected.decision.next_wake_after_ms;
            if (!requested_delay && inspected.decision.decision == "continue_local")
                requested_delay = local_goose_cycle_minimum_delay_ms;

            if (requested_delay) {
                std::int64_t next_due = 0;
                if (!add_delay(now, *requested_delay, next_due)) {
                    return write_blocked(cycle_store_, cursor,
                        "next Local Goose cycle deadline overflows", task);
                }
                replacement.state = LocalGooseCycleState::scheduled;
                replacement.due_at_ms = next_due;
            } else {
                replacement.state = LocalGooseCycleState::dormant;
                replacement.due_at_ms.reset();
            }

            if (!valid_local_goose_cycle_transition(cursor, replacement)) {
                return write_blocked(cycle_store_, cursor,
                    "settled Local Goose cycle transition is non-canonical", task);
            }
            const auto write = cycle_store_.replace(cursor, replacement);
            if (write.result == StoreResult::accepted
                || write.result == StoreResult::duplicate) {
                const auto result = replacement.state == LocalGooseCycleState::scheduled
                    ? ServiceResult::scheduled : ServiceResult::dormant;
                return {true, replacement.state == LocalGooseCycleState::scheduled,
                        result,
                        write.cursor ? write.cursor
                                     : std::optional<Cursor>{replacement},
                        task, inspected.decision, {}};
            }
            if (write.result == StoreResult::unavailable) {
                return {false, true, ServiceResult::unavailable,
                        write.cursor, task, inspected.decision, write.detail};
            }
            return {false, false, ServiceResult::conflict,
                    write.cursor, task, inspected.decision,
                    write.detail.empty() ? "Local Goose cycle settlement CAS conflict"
                                         : write.detail};
        }

        if (task->status != TaskStatus::pending) {
            return {true, true, ServiceResult::waiting,
                    cursor, task, {},
                    "Local Goose cycle Task is active or cancellation-pending"};
        }
        if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
            return {true, true, ServiceResult::unavailable,
                    cursor, task, {}, "work runtime is not running for cycle execution"};
        }

        TaskExecutor executor(work_runtime_, task_store_);
        const auto executed = executor.execute(
            task->id, "local-goose-cycle", handler_);
        if (executed == ExecuteResult::state_conflict) {
            return write_blocked(cycle_store_, cursor,
                "Local Goose cycle execution encountered state conflict", task);
        }
        if (executed == ExecuteResult::not_startable) {
            return {true, true, ServiceResult::waiting,
                    cursor, task, {}, "Local Goose cycle Task is not startable yet"};
        }

        task = task_store_.find(expected->id);
        if (!task) {
            return write_blocked(cycle_store_, cursor,
                "executed Local Goose cycle Task disappeared");
        }
        return {true, true, ServiceResult::executed,
                cursor, task, {}, {}};
    } catch (const std::exception& error) {
        return {false, false, ServiceResult::unavailable,
                {}, {}, {}, error.what()};
    }
}

} // namespace gaudere_agent
