#include "AutonomousCognitionProviderGate.hpp"

#include "CurrentCognitionCycle.hpp"
#include "CurrentCognitionTaskInspection.hpp"
#include "OpenAIBudget.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using GateResult = AutonomousCognitionProviderGateResult;
using Observation = AutonomousCognitionProviderGateObservation;
using TaskStatus = gaudere::work::TaskStatus;

struct NonterminalSelection {
    bool available = false;
    std::vector<std::string> ids;
    std::string detail;
};

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
    }

    ~Statement() { sqlite3_finalize(statement_); }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

std::string column_text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const auto bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

NonterminalSelection select_nonterminal_current_tasks(
    const std::string& state_path) noexcept
{
    sqlite3* database = nullptr;
    try {
        if (state_path.empty())
            return {false, {}, "state database path is empty"};
        if (sqlite3_open_v2(state_path.c_str(), &database,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database ? sqlite3_errmsg(database)
                                                 : "cannot open state database read-only";
            sqlite3_close(database);
            return {false, {}, message};
        }
        sqlite3_busy_timeout(database, 5000);
        Statement statement(database,
            "SELECT id FROM tasks "
            "WHERE kind=?1 AND status BETWEEN ?2 AND ?3 "
            "ORDER BY id LIMIT 2");
        if (sqlite3_bind_text(statement.get(), 1, current_cognition_task_kind, -1,
                              SQLITE_STATIC) != SQLITE_OK
            || sqlite3_bind_int(statement.get(), 2,
                static_cast<int>(TaskStatus::pending)) != SQLITE_OK
            || sqlite3_bind_int(statement.get(), 3,
                static_cast<int>(TaskStatus::cancel_requested)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }

        std::vector<std::string> ids;
        while (true) {
            const auto step = sqlite3_step(statement.get());
            if (step == SQLITE_DONE) break;
            if (step != SQLITE_ROW)
                throw std::runtime_error(sqlite3_errmsg(database));
            const auto id = column_text(statement.get(), 0);
            if (id.empty())
                throw std::runtime_error("non-terminal current cognition has empty id");
            ids.push_back(id);
            if (ids.size() == 2) break;
        }
        sqlite3_close(database);
        database = nullptr;
        return {true, std::move(ids), {}};
    } catch (const std::exception& error) {
        sqlite3_close(database);
        return {false, {}, error.what()};
    } catch (...) {
        sqlite3_close(database);
        return {false, {}, "current cognition selector failed"};
    }
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

Observation waiting(std::string detail,
                    std::optional<gaudere::work::TimePoint> retry_at = std::nullopt)
{
    return {GateResult::waiting, {}, retry_at, std::move(detail)};
}

Observation blocked(std::string detail)
{
    return {GateResult::blocked, {}, {}, std::move(detail)};
}

Observation unavailable(std::string detail)
{
    return {GateResult::unavailable, {}, {}, std::move(detail)};
}

} // namespace

AutonomousCognitionProviderGate::AutonomousCognitionProviderGate(
    std::string state_path,
    gaudere::work::TaskStore& task_store,
    gaudere::budget::Store& budget_store,
    gaudere::scheduling::wake::ActionStore& action_store,
    Now now)
    : state_path_(std::move(state_path)),
      task_store_(task_store), budget_store_(budget_store),
      action_store_(action_store), now_(std::move(now))
{
    if (state_path_.empty())
        throw std::invalid_argument("autonomous provider gate state path is required");
    if (!now_)
        throw std::invalid_argument("autonomous provider gate clock is required");
}

AutonomousCognitionProviderGateObservation
AutonomousCognitionProviderGate::evaluate(
    const AutonomousCognitionPulseCursor& cursor) const
{
    try {
        if (!valid_autonomous_cognition_pulse_cursor(cursor))
            return blocked("autonomous pulse cursor is non-canonical");

        switch (cursor.state) {
        case AutonomousCognitionPulseState::idle:
            return waiting("autonomous pulse is idle");
        case AutonomousCognitionPulseState::quiescent:
            return waiting("autonomous pulse is quiescent");
        case AutonomousCognitionPulseState::preparing:
            return waiting("autonomous pulse still owns cognition preparation");
        case AutonomousCognitionPulseState::blocked:
            return blocked("autonomous pulse is blocked: " + cursor.blocked_reason);
        case AutonomousCognitionPulseState::prepared:
            break;
        }

        if (!cursor.observed_at_ms || *cursor.observed_at_ms < cursor.due_at_ms)
            return blocked("prepared pulse observation is inconsistent with its deadline");

        const auto task = task_store_.find(cursor.current_task_id);
        if (!task)
            return blocked("pulse-prepared current cognition Task is missing");
        if (!valid_current_cognition_task(*task))
            return blocked("pulse-prepared current cognition Task is non-canonical");

        if (gaudere::work::is_terminal(task->status)) {
            if (task->status == TaskStatus::succeeded)
                return waiting("prepared cognition is terminal; pulse settlement owns the next transition");
            return blocked("prepared cognition reached a non-success terminal state");
        }
        if (task->status != TaskStatus::pending)
            return blocked("prepared cognition has ambiguous execution ownership");
        if (task->attempts_started >= task->limits.max_attempts)
            return blocked("prepared cognition exhausted its execution attempts");

        const auto lineage = inspect_current_cognition_task(*task);
        if (!lineage.eligible)
            return blocked("current cognition lineage is unavailable: " + lineage.detail);
        if (lineage.predecessor_task_id != cursor.predecessor_task_id)
            return blocked("current cognition predecessor differs from pulse cursor");
        if (lineage.snapshot_task_id != cursor.snapshot_task_id)
            return blocked("current cognition snapshot differs from pulse cursor");
        if (lineage.captured_at_ms != *cursor.observed_at_ms)
            return blocked("current cognition capture time differs from frozen pulse observation");

        const auto snapshot = task_store_.find(cursor.snapshot_task_id);
        if (!snapshot)
            return blocked("pulse current-context snapshot Task is missing");
        const auto snapshot_inspection = inspect_resume_context_snapshot(*snapshot);
        if (!snapshot_inspection.eligible)
            return blocked("pulse current-context snapshot is non-canonical: "
                           + snapshot_inspection.detail);
        if (snapshot_inspection.canonical_capsule != lineage.snapshot_capsule
            || snapshot_inspection.captured_at_ms != lineage.captured_at_ms) {
            return blocked("durable snapshot differs from current cognition linkage");
        }

        const auto predecessor = task_store_.find(cursor.predecessor_task_id);
        if (!predecessor || predecessor->kind != current_cognition_task_kind
            || !valid_current_cognition_task(*predecessor)
            || predecessor->status != TaskStatus::succeeded
            || !predecessor->result
            || predecessor->result->content_type
                != resume_after_wake_decision_content_type) {
            return blocked("pulse predecessor is no longer canonical succeeded current cognition");
        }
        if (sha256_hex(predecessor->result->output)
            != cursor.predecessor_result_sha256) {
            return blocked("pulse predecessor result hash drifted");
        }
        if (predecessor->result->output != lineage.predecessor_decision)
            return blocked("current cognition embedded predecessor decision drifted");

        const auto singleton = select_nonterminal_current_tasks(state_path_);
        if (!singleton.available)
            return unavailable("current cognition singleton selector unavailable: "
                               + singleton.detail);
        if (singleton.ids.size() != 1 || singleton.ids.front() != task->id)
            return blocked("non-terminal current cognition authority is ambiguous");

        const auto now = now_();
        const auto now_ms = milliseconds(now);
        if (now_ms < lineage.captured_at_ms)
            return blocked("provider gate clock precedes frozen current context");
        const auto age_ms = now_ms - lineage.captured_at_ms;
        const auto max_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_cognition_max_snapshot_age).count();
        if (age_ms > max_age_ms)
            return blocked("pulse-prepared current context is stale at provider boundary");

        // Durable effect evidence dominates budget availability. A previous Action
        // must never be hidden by cooldown/window/total classification because that
        // could later turn into an accidental replay when the budget opens again.
        const auto expected_action_key = std::string{openai_budget_scope()}
            + ":" + task->idempotency_key;
        const auto expected_action_id = std::string{openai_budget_scope()}
            + ":" + task->id;
        if (action_store_.find_by_idempotency_key(expected_action_key)
            || action_store_.find(expected_action_id)) {
            return blocked("provider Action already exists for non-terminal cognition; replay forbidden");
        }

        const auto policy = openai_bootstrap_budget_policy();
        const auto budget = budget_store_.snapshot(
            std::string{openai_budget_scope()}, now, policy);
        switch (budget.next_new_consumption) {
        case gaudere::budget::ConsumeResult::accepted:
            break;
        case gaudere::budget::ConsumeResult::cooldown: {
            std::optional<gaudere::work::TimePoint> retry;
            if (budget.last_consumed_at)
                retry = *budget.last_consumed_at + policy.min_interval;
            return waiting("provider budget is in cooldown", retry);
        }
        case gaudere::budget::ConsumeResult::window_exhausted:
            return waiting("provider rolling-window budget is exhausted");
        case gaudere::budget::ConsumeResult::total_exhausted:
            return {GateResult::dormant, {}, {},
                    "provider lifetime budget is exhausted"};
        case gaudere::budget::ConsumeResult::clock_rollback:
            return blocked("provider budget detected clock rollback");
        case gaudere::budget::ConsumeResult::duplicate:
            return blocked("read-only provider budget snapshot returned duplicate");
        }

        return {GateResult::eligible, task->id, {}, {}};
    } catch (const std::exception& error) {
        return unavailable(error.what());
    } catch (...) {
        return unavailable("autonomous provider gate failed");
    }
}

} // namespace gaudere_agent
