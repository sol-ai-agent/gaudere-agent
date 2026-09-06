#include "LocalActivityPulse.hpp"

#include "ContinuityDeltaCheckpointInspection.hpp"
#include "LocalContinuityObservation.hpp"
#include "LocalContinuityObservationHandler.hpp"
#include "OpenAIBudget.hpp"
#include "Sha256.hpp"
#include "TaskExecutor.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using StoreResult = LocalActivityPulseStoreResult;
using PulseResult = LocalActivityPulseResult;
using CheckpointInspection = ContinuityDeltaCheckpointInspection;

struct AnchorEvidence {
    std::optional<Task> task;
    CheckpointInspection inspection;
    std::string detail;
};

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

gaudere::work::TimePoint time_point(const std::int64_t value)
{
    return gaudere::work::TimePoint{std::chrono::milliseconds{value}};
}

bool add_cadence(const std::int64_t base, std::int64_t& result) noexcept
{
    if (base < 0
        || base > std::numeric_limits<std::int64_t>::max()
            - local_activity_pulse_cadence_ms) {
        return false;
    }
    result = base + local_activity_pulse_cadence_ms;
    return true;
}

bool same_limits(const gaudere::work::ResourceLimits& left,
                 const gaudere::work::ResourceLimits& right) noexcept
{
    return left.max_input_bytes == right.max_input_bytes
        && left.max_output_bytes == right.max_output_bytes
        && left.max_runtime == right.max_runtime
        && left.max_attempts == right.max_attempts;
}

bool same_definition(const Task& left, const Task& right) noexcept
{
    return left.id == right.id
        && left.idempotency_key == right.idempotency_key
        && left.kind == right.kind
        && left.input_content_type == right.input_content_type
        && left.input == right.input
        && same_limits(left.limits, right.limits);
}

const char* wake_status_name(
    const gaudere::scheduling::wake::WakeIntentStatus status) noexcept
{
    using Status = gaudere::scheduling::wake::WakeIntentStatus;
    switch (status) {
    case Status::scheduled: return "scheduled";
    case Status::fired: return "fired";
    case Status::revoked: return "revoked";
    case Status::manual_review: return "manual_review";
    }
    return "unknown";
}

std::string current_wake_canonical(
    gaudere::scheduling::wake::WakeIntentStore& wake_store,
    const std::string& scope)
{
    const auto inspected = wake_store.inspect_scope(scope);
    using Result = gaudere::scheduling::wake::WakeIntentScopeResult;
    switch (inspected.result) {
    case Result::empty:
        return Json{{"scope", scope}, {"cardinality", "empty"}}.dump();
    case Result::ambiguous:
        throw std::runtime_error("historical WakeIntent scope became ambiguous");
    case Result::one:
        if (!inspected.intent)
            throw std::runtime_error("WakeIntent scope reported one row without intent");
        return Json{
            {"scope", scope},
            {"cardinality", "one"},
            {"id", inspected.intent->id},
            {"source_id", inspected.intent->source_id},
            {"status", wake_status_name(inspected.intent->status)},
            {"accepted_at_ms", milliseconds(inspected.intent->accepted_at)},
            {"due_at_ms", milliseconds(inspected.intent->due_at)}
        }.dump();
    }
    throw std::runtime_error("unknown WakeIntent scope result");
}

AnchorEvidence inspect_anchor(gaudere::work::TaskStore& task_store,
                              const std::string& task_id,
                              const std::optional<std::string>& required_hash)
{
    AnchorEvidence evidence;
    evidence.task = task_store.find(task_id);
    if (!evidence.task) {
        evidence.detail = "anchor continuity checkpoint Task is missing";
        return evidence;
    }
    evidence.inspection =
        inspect_succeeded_continuity_delta_checkpoint(*evidence.task);
    if (!evidence.inspection.eligible) {
        evidence.detail = "anchor continuity checkpoint is non-canonical: "
            + evidence.inspection.detail;
        return evidence;
    }
    if (required_hash
        && evidence.inspection.checkpoint_result_sha256 != *required_hash) {
        evidence.detail = "anchor checkpoint result hash differs from pulse cursor";
        evidence.inspection.eligible = false;
    }
    return evidence;
}

void require_confirmed_action(
    gaudere::scheduling::wake::ActionStore& action_store,
    const std::string& id,
    const std::string& key)
{
    const auto by_id = action_store.find(id);
    const auto by_key = action_store.find_by_idempotency_key(key);
    if (!by_id || !by_key || by_id->id != by_key->id
        || by_id->id != id || by_id->idempotency_key != key
        || by_id->status != gaudere::scheduling::wake::ActionStatus::succeeded
        || by_id->effect_result != gaudere::scheduling::wake::EffectResult::confirmed
        || by_id->lease.has_value()) {
        throw std::runtime_error(
            "anchored provider Action is missing, ambiguous or no longer confirmed");
    }
}

LocalContinuityObservationFacts capture_facts(
    const LocalActivityPulseCursor& cursor,
    const CheckpointInspection& checkpoint,
    gaudere::scheduling::wake::ActionStore& action_store,
    gaudere::budget::Store& budget_store,
    gaudere::scheduling::wake::WakeIntentStore& wake_store)
{
    if (!cursor.captured_at_ms)
        throw std::invalid_argument("preparing cursor has no frozen capture time");

    require_confirmed_action(action_store,
                             checkpoint.predecessor_provider_action_id,
                             checkpoint.predecessor_provider_action_key);
    require_confirmed_action(action_store,
                             checkpoint.audited_provider_action_id,
                             checkpoint.audited_provider_action_key);

    const auto policy = openai_bootstrap_budget_policy();
    const auto budget = budget_store.snapshot(
        checkpoint.provider_scope,
        time_point(*cursor.captured_at_ms),
        policy);
    if (budget.total_used != checkpoint.provider_total) {
        throw std::runtime_error(
            "provider budget moved after the anchored continuity checkpoint");
    }

    const auto wake = current_wake_canonical(
        wake_store, checkpoint.historical_wake_scope);
    if (wake != checkpoint.historical_wake_canonical
        || sha256_hex(wake) != checkpoint.historical_wake_sha256) {
        throw std::runtime_error(
            "historical WakeIntent evidence changed after the anchor checkpoint");
    }

    LocalContinuityObservationFacts facts;
    facts.generation = static_cast<std::uint32_t>(cursor.generation);
    facts.due_at_ms = cursor.due_at_ms;
    facts.captured_at_ms = *cursor.captured_at_ms;
    facts.predecessor_observation_task_id =
        cursor.predecessor_observation_task_id;
    facts.predecessor_observation_result_sha256 =
        cursor.predecessor_observation_result_sha256;
    facts.anchor_checkpoint_task_id = cursor.anchor_checkpoint_task_id;
    facts.anchor_checkpoint_result_sha256 =
        cursor.anchor_checkpoint_result_sha256;
    facts.provider_scope = checkpoint.provider_scope;
    facts.provider_total = budget.total_used;
    facts.provider_limit = policy.max_total;
    facts.predecessor_provider_action_id =
        checkpoint.predecessor_provider_action_id;
    facts.audited_provider_action_id = checkpoint.audited_provider_action_id;
    facts.historical_wake_scope = checkpoint.historical_wake_scope;
    facts.historical_wake_sha256 = checkpoint.historical_wake_sha256;
    return facts;
}

LocalActivityPulseObservation from_store_write(
    const LocalActivityPulseStoreWrite& write,
    const PulseResult accepted,
    const std::string& conflict_detail)
{
    switch (write.result) {
    case StoreResult::accepted:
        return {accepted, write.cursor, {}, {}};
    case StoreResult::duplicate:
        return {PulseResult::duplicate, write.cursor, {}, {}};
    case StoreResult::conflict:
        return {PulseResult::conflict, write.cursor, {},
                write.detail.empty() ? conflict_detail : write.detail};
    case StoreResult::invalid:
        return {PulseResult::conflict, write.cursor, {},
                write.detail.empty() ? "local pulse sidecar rejected cursor"
                                     : write.detail};
    case StoreResult::unavailable:
        return {PulseResult::unavailable, write.cursor, {}, write.detail};
    }
    return {PulseResult::unavailable, {}, {}, "unknown local pulse sidecar result"};
}

LocalActivityPulseObservation block_cursor(
    LocalActivityPulseStore& store,
    const LocalActivityPulseCursor& cursor,
    const std::string& reason,
    const std::optional<Task>& task = std::nullopt)
{
    auto blocked = cursor;
    ++blocked.revision;
    blocked.state = LocalActivityPulseState::blocked;
    blocked.blocked_reason = reason;
    const auto write = store.replace(cursor, blocked);
    if (write.result == StoreResult::accepted
        || write.result == StoreResult::duplicate) {
        return {PulseResult::blocked, write.cursor, task, reason};
    }
    return from_store_write(write, PulseResult::blocked,
                            "could not persist blocked local pulse cursor");
}

LocalActivityPulseObservation settle_cursor(
    LocalActivityPulseStore& store,
    const LocalActivityPulseCursor& cursor,
    const Task& task,
    const LocalActivityPulse::PhaseHook& phase_hook)
{
    if (!canonical_local_continuity_observation_success(task) || !task.result)
        return block_cursor(store, cursor,
                            "local observation Task is not canonical succeeded", task);

    auto settled = cursor;
    ++settled.revision;
    settled.result_sha256 = sha256_hex(task.result->output);
    settled.blocked_reason.clear();
    settled.state = cursor.generation == 3
        ? LocalActivityPulseState::quiescent
        : LocalActivityPulseState::settled;
    const auto write = store.replace(cursor, settled);
    if (write.result != StoreResult::accepted
        && write.result != StoreResult::duplicate) {
        auto result = from_store_write(write, PulseResult::settled,
                                       "local pulse settlement conflict");
        result.task = task;
        return result;
    }
    if (write.result == StoreResult::accepted && phase_hook)
        phase_hook("after_settlement");
    const auto final_state = write.cursor ? write.cursor->state : settled.state;
    return {final_state == LocalActivityPulseState::quiescent
                ? PulseResult::quiescent : PulseResult::settled,
            write.cursor, task, {}};
}

} // namespace

LocalActivityPulse::LocalActivityPulse(
    LocalActivityPulseStore& pulse_store,
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::ActionStore& action_store,
    gaudere::budget::Store& budget_store,
    gaudere::scheduling::wake::WakeIntentStore& wake_store,
    gaudere::work::Runtime& work_runtime,
    Now now,
    const bool enabled,
    PhaseHook phase_hook)
    : pulse_store_(pulse_store), task_store_(task_store),
      action_store_(action_store), budget_store_(budget_store),
      wake_store_(wake_store), work_runtime_(work_runtime),
      now_(std::move(now)), enabled_(enabled),
      phase_hook_(std::move(phase_hook))
{
    if (!now_) throw std::invalid_argument("local activity pulse clock is required");
}

LocalActivityPulseObservation LocalActivityPulse::seed(
    const std::string& checkpoint_task_id)
{
    if (!enabled_)
        return {PulseResult::disabled, {}, {}, "local activity pulse is disabled"};
    try {
        const auto anchor = inspect_anchor(task_store_, checkpoint_task_id, std::nullopt);
        if (!anchor.inspection.eligible) {
            return {PulseResult::ineligible, {}, anchor.task, anchor.detail};
        }

        const auto existing = pulse_store_.find(local_activity_pulse_scope);
        if (existing) {
            if (existing->anchor_checkpoint_task_id == checkpoint_task_id
                && existing->anchor_checkpoint_result_sha256
                    == anchor.inspection.checkpoint_result_sha256) {
                return {PulseResult::duplicate, existing, anchor.task,
                        "local activity pulse is already seeded from this checkpoint"};
            }
            return {PulseResult::conflict, existing, anchor.task,
                    "local activity pulse is already seeded from another checkpoint"};
        }

        const auto now_ms = milliseconds(now_());
        if (now_ms < 0)
            return {PulseResult::clock_rollback, {}, anchor.task,
                    "local activity seed clock precedes Unix epoch"};
        std::int64_t due_at_ms = 0;
        if (!add_cadence(now_ms, due_at_ms))
            return {PulseResult::conflict, {}, anchor.task,
                    "local activity seed deadline overflows"};

        LocalActivityPulseCursor cursor;
        cursor.anchor_checkpoint_task_id = checkpoint_task_id;
        cursor.anchor_checkpoint_result_sha256 =
            anchor.inspection.checkpoint_result_sha256;
        cursor.anchor_at_ms = now_ms;
        cursor.due_at_ms = due_at_ms;
        const auto write = pulse_store_.seed(cursor);
        auto result = from_store_write(
            write, PulseResult::seeded, "local activity pulse seed conflict");
        result.task = anchor.task;
        return result;
    } catch (const std::exception& error) {
        return {PulseResult::unavailable, {}, {}, error.what()};
    }
}

LocalActivityPulseObservation LocalActivityPulse::observe()
{
    if (!enabled_)
        return {PulseResult::disabled, {}, {}, "local activity pulse is disabled"};
    try {
        auto found = pulse_store_.find(local_activity_pulse_scope);
        if (!found)
            return {PulseResult::unseeded, {}, {}, "local activity pulse is unseeded"};
        auto cursor = *found;
        if (cursor.state == LocalActivityPulseState::blocked)
            return {PulseResult::blocked, cursor, {}, cursor.blocked_reason};
        if (cursor.state == LocalActivityPulseState::quiescent)
            return {PulseResult::quiescent, cursor, {}, {}};

        const auto now_ms = milliseconds(now_());
        if (now_ms < cursor.anchor_at_ms
            || (cursor.captured_at_ms && now_ms < *cursor.captured_at_ms)) {
            return {PulseResult::clock_rollback, cursor, {},
                    "local activity clock precedes durable anchor/capture"};
        }

        if (cursor.state == LocalActivityPulseState::idle
            || cursor.state == LocalActivityPulseState::settled) {
            std::uint64_t generation = 1;
            std::int64_t due_at_ms = cursor.due_at_ms;
            std::optional<std::string> predecessor_task;
            std::optional<std::string> predecessor_hash;

            if (cursor.state == LocalActivityPulseState::settled) {
                if (!cursor.captured_at_ms || !cursor.result_sha256
                    || cursor.task_id.empty() || cursor.generation >= 3) {
                    return block_cursor(pulse_store_, cursor,
                                        "settled local pulse cursor is incomplete");
                }
                generation = cursor.generation + 1;
                if (!add_cadence(*cursor.captured_at_ms, due_at_ms))
                    return block_cursor(pulse_store_, cursor,
                                        "next local observation deadline overflows");
                predecessor_task = cursor.task_id;
                predecessor_hash = cursor.result_sha256;
            }

            if (now_ms < due_at_ms)
                return {PulseResult::not_due, cursor, {}, {}};
            if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
                return {PulseResult::unavailable, cursor, {},
                        "work runtime is not running at due admission"};
            }

            LocalContinuityObservationOpportunity opportunity;
            opportunity.generation = static_cast<std::uint32_t>(generation);
            opportunity.due_at_ms = due_at_ms;
            opportunity.predecessor_observation_task_id = predecessor_task;
            opportunity.predecessor_observation_result_sha256 = predecessor_hash;
            opportunity.anchor_checkpoint_task_id =
                cursor.anchor_checkpoint_task_id;
            opportunity.anchor_checkpoint_result_sha256 =
                cursor.anchor_checkpoint_result_sha256;

            auto preparing = cursor;
            ++preparing.revision;
            preparing.generation = generation;
            preparing.state = LocalActivityPulseState::preparing;
            preparing.due_at_ms = due_at_ms;
            preparing.captured_at_ms = now_ms;
            preparing.task_id = local_continuity_observation_task_id(opportunity);
            preparing.result_sha256.reset();
            preparing.predecessor_observation_task_id = predecessor_task;
            preparing.predecessor_observation_result_sha256 = predecessor_hash;
            preparing.blocked_reason.clear();

            const auto write = pulse_store_.replace(cursor, preparing);
            if (write.result != StoreResult::accepted
                && write.result != StoreResult::duplicate) {
                return from_store_write(write, PulseResult::preparing,
                                        "local activity due admission conflict");
            }
            if (!write.cursor)
                return {PulseResult::unavailable, {}, {},
                        "local activity admission returned no cursor"};
            cursor = *write.cursor;
            if (write.result == StoreResult::accepted && phase_hook_)
                phase_hook_("after_preparing");
        }

        if (cursor.state != LocalActivityPulseState::preparing
            || !cursor.captured_at_ms || cursor.task_id.empty()) {
            return {PulseResult::conflict, cursor, {},
                    "local activity cursor is not recoverably preparing"};
        }
        if (now_ms < *cursor.captured_at_ms)
            return {PulseResult::clock_rollback, cursor, {},
                    "local activity clock precedes frozen capture"};

        const auto anchor = inspect_anchor(
            task_store_, cursor.anchor_checkpoint_task_id,
            cursor.anchor_checkpoint_result_sha256);
        if (!anchor.inspection.eligible)
            return block_cursor(pulse_store_, cursor, anchor.detail, anchor.task);

        LocalContinuityObservationFacts facts;
        try {
            facts = capture_facts(cursor, anchor.inspection,
                                  action_store_, budget_store_, wake_store_);
        } catch (const std::exception& error) {
            return block_cursor(pulse_store_, cursor, error.what(), anchor.task);
        }

        Task expected;
        try {
            expected = make_local_continuity_observation_task(facts);
        } catch (const std::exception& error) {
            return block_cursor(pulse_store_, cursor,
                                "could not construct canonical observation: "
                                    + std::string{error.what()});
        }
        if (expected.id != cursor.task_id)
            return block_cursor(pulse_store_, cursor,
                                "reserved observation Task identity changed after capture");

        auto by_id = task_store_.find(expected.id);
        auto by_key = task_store_.find_by_idempotency_key(expected.idempotency_key);
        std::optional<Task> task;
        if (by_id || by_key) {
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, expected)) {
                return block_cursor(pulse_store_, cursor,
                                    "deterministic local observation Task conflicts",
                                    by_id ? by_id : by_key);
            }
            task = by_id;
        } else {
            if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
                return {PulseResult::unavailable, cursor, {},
                        "work runtime is not running during observation recovery"};
            }
            const auto submitted = work_runtime_.submit(expected);
            if (submitted == gaudere::work::SubmitResult::invalid)
                return block_cursor(pulse_store_, cursor,
                                    "work runtime rejected canonical local observation");
            if (submitted == gaudere::work::SubmitResult::unavailable)
                return {PulseResult::unavailable, cursor, {},
                        "work runtime could not submit local observation"};
            if (submitted == gaudere::work::SubmitResult::accepted && phase_hook_)
                phase_hook_("after_submit");

            by_id = task_store_.find(expected.id);
            by_key = task_store_.find_by_idempotency_key(expected.idempotency_key);
            if (!by_id || !by_key || by_id->id != by_key->id
                || !same_definition(*by_id, expected)) {
                return block_cursor(pulse_store_, cursor,
                                    "submitted local observation did not persist exactly",
                                    by_id ? by_id : by_key);
            }
            task = by_id;
        }

        if (!task)
            return {PulseResult::unavailable, cursor, {},
                    "local observation Task lookup returned no Task"};
        if (gaudere::work::is_terminal(task->status)) {
            if (task->status != TaskStatus::succeeded
                || !canonical_local_continuity_observation_success(*task)) {
                return block_cursor(pulse_store_, cursor,
                                    "local observation reached non-canonical terminal state",
                                    task);
            }
            return settle_cursor(pulse_store_, cursor, *task, phase_hook_);
        }

        if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
            return {PulseResult::unavailable, cursor, task,
                    "work runtime is not running during local observation execution"};
        }

        LocalContinuityObservationHandler handler;
        TaskExecutor executor(work_runtime_, task_store_);
        const auto executed = executor.execute(
            task->id, "local-continuity-observation", handler);
        if (executed == ExecuteResult::not_startable)
            return {PulseResult::waiting, cursor, task,
                    "local observation Task is already active or not startable"};
        if (executed == ExecuteResult::state_conflict)
            return block_cursor(pulse_store_, cursor,
                                "local observation execution encountered state conflict",
                                task);
        if (phase_hook_) phase_hook_("after_execute");

        task = task_store_.find(expected.id);
        if (!task)
            return block_cursor(pulse_store_, cursor,
                                "executed local observation Task disappeared");
        if (!gaudere::work::is_terminal(task->status))
            return {PulseResult::waiting, cursor, task,
                    "local observation Task remains non-terminal"};
        if (task->status != TaskStatus::succeeded
            || !canonical_local_continuity_observation_success(*task)) {
            return block_cursor(pulse_store_, cursor,
                                "executed local observation did not succeed canonically",
                                task);
        }
        return settle_cursor(pulse_store_, cursor, *task, phase_hook_);
    } catch (const std::exception& error) {
        return {PulseResult::unavailable, {}, {}, error.what()};
    }
}

} // namespace gaudere_agent
