#include "AutonomousCognitionStaleRefresh.hpp"

#include "CurrentCognitionCycle.hpp"
#include "OpenAIBudget.hpp"

#include <utility>

namespace gaudere_agent {
namespace {

using RefreshResult = AutonomousCognitionStaleRefreshResult;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using StoreResult = AutonomousCognitionPulseStoreResult;

bool exact_retirement_marker(const Task& task) noexcept
{
    return task.status == TaskStatus::cancelled
        && task.attempts_started == 0
        && task.cancel_reason == autonomous_cognition_stale_retirement_reason
        && task.result
        && task.result->failure_code == "cancelled"
        && task.result->failure_message
            == autonomous_cognition_stale_retirement_reason;
}

} // namespace

AutonomousCognitionStaleRefresh::AutonomousCognitionStaleRefresh(
    AutonomousCognitionPulseStore& pulse_store,
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::ActionStore& action_store,
    gaudere::work::Runtime& work_runtime,
    AutonomousCognitionProviderGate& provider_gate)
    : pulse_store_(pulse_store), task_store_(task_store),
      action_store_(action_store), work_runtime_(work_runtime),
      provider_gate_(provider_gate)
{
}

bool AutonomousCognitionStaleRefresh::provider_action_absent(
    const Task& task) const
{
    const auto expected_action_id = std::string{openai_budget_scope()}
        + ":" + task.id;
    const auto expected_action_key = std::string{openai_budget_scope()}
        + ":" + task.idempotency_key;
    return !action_store_.find(expected_action_id)
        && !action_store_.find_by_idempotency_key(expected_action_key);
}

AutonomousCognitionStaleRefreshObservation
AutonomousCognitionStaleRefresh::reset_retired(
    const AutonomousCognitionPulseCursor& cursor,
    const Task& task)
{
    if (!exact_retirement_marker(task)) {
        return {RefreshResult::blocked, cursor, task,
                "stale cognition cancellation marker is non-canonical"};
    }
    if (!provider_action_absent(task)) {
        return {RefreshResult::blocked, cursor, task,
                "provider Action exists for stale-retired cognition; reset forbidden"};
    }

    auto reset = cursor;
    ++reset.revision;
    reset.state = AutonomousCognitionPulseState::idle;
    reset.observed_at_ms.reset();
    reset.snapshot_task_id.clear();
    reset.current_task_id.clear();
    reset.blocked_reason.clear();

    const auto write = pulse_store_.replace(cursor, reset);
    switch (write.result) {
    case StoreResult::accepted:
    case StoreResult::duplicate:
        return {RefreshResult::retired, write.cursor, task,
                "stale unspent cognition retired; same generation is due for fresh preparation"};
    case StoreResult::conflict:
    case StoreResult::invalid:
        return {RefreshResult::blocked, write.cursor, task,
                write.detail.empty()
                    ? "stale cognition cursor reset conflicted"
                    : write.detail};
    case StoreResult::unavailable:
        return {RefreshResult::unavailable, write.cursor, task, write.detail};
    }
    return {RefreshResult::unavailable, {}, task,
            "unknown stale cognition cursor reset result"};
}

AutonomousCognitionStaleRefreshObservation
AutonomousCognitionStaleRefresh::step()
{
    try {
        const auto found = pulse_store_.find(autonomous_cognition_pulse_scope);
        if (!found) {
            return {RefreshResult::not_applicable, {}, {},
                    "autonomous pulse is unseeded"};
        }
        const auto cursor = *found;
        if (!valid_autonomous_cognition_pulse_cursor(cursor)) {
            return {RefreshResult::blocked, cursor, {},
                    "autonomous pulse cursor is non-canonical"};
        }
        if (cursor.state != AutonomousCognitionPulseState::prepared) {
            return {RefreshResult::not_applicable, cursor, {}, {}};
        }

        const auto task = task_store_.find(cursor.current_task_id);
        if (!task) {
            return {RefreshResult::blocked, cursor, {},
                    "prepared cognition Task is missing"};
        }

        // Crash recovery: Runtime::request_cancel() persists the exact terminal
        // marker before the cursor is replaced. A restart may therefore finish only
        // the reset without re-running the provider gate or creating another Task.
        if (exact_retirement_marker(*task)) {
            return reset_retired(cursor, *task);
        }

        const auto gate = provider_gate_.evaluate(cursor);
        if (gate.result != AutonomousCognitionProviderGateResult::blocked
            || gate.detail != autonomous_cognition_provider_stale_detail) {
            if (gate.result == AutonomousCognitionProviderGateResult::unavailable) {
                return {RefreshResult::unavailable, cursor, task, gate.detail};
            }
            return {RefreshResult::not_applicable, cursor, task, gate.detail};
        }
        if (!gate.task_id && task->id != cursor.current_task_id) {
            return {RefreshResult::blocked, cursor, task,
                    "stale gate did not preserve exact pulse Task identity"};
        }
        if (task->status != TaskStatus::pending || task->attempts_started != 0) {
            return {RefreshResult::blocked, cursor, task,
                    "stale cognition has execution evidence; retirement forbidden"};
        }
        if (!provider_action_absent(*task)) {
            return {RefreshResult::blocked, cursor, task,
                    "provider Action exists for stale cognition; retirement forbidden"};
        }

        if (!work_runtime_.request_cancel(
                task->id, autonomous_cognition_stale_retirement_reason)) {
            const auto raced = task_store_.find(task->id);
            if (raced && exact_retirement_marker(*raced)) {
                return reset_retired(cursor, *raced);
            }
            return {RefreshResult::blocked, cursor, raced,
                    "stale cognition could not be cancelled through work Runtime"};
        }

        const auto retired = task_store_.find(task->id);
        if (!retired) {
            return {RefreshResult::unavailable, cursor, {},
                    "stale cognition disappeared after cancellation"};
        }
        return reset_retired(cursor, *retired);
    } catch (const std::exception& error) {
        return {RefreshResult::unavailable, {}, {}, error.what()};
    } catch (...) {
        return {RefreshResult::unavailable, {}, {},
                "stale cognition refresh failed"};
    }
}

} // namespace gaudere_agent
