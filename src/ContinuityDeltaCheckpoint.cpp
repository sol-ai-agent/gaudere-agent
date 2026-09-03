#include "ContinuityDeltaCheckpoint.hpp"

#include "CanonicalCognitionDecision.hpp"
#include "CurrentCognitionCycle.hpp"
#include "CurrentCognitionTaskInspection.hpp"
#include "OpenAIBudget.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "Sha256.hpp"
#include "TaskExecutor.hpp"
#include "WakeSourceDecision.hpp"

#include <gaudere/scheduling/wake/Action.hpp>
#include <gaudere/scheduling/wake/WakeIntent.hpp>
#include <gaudere/work/Task.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using Action = gaudere::scheduling::wake::Action;
using ActionStatus = gaudere::scheduling::wake::ActionStatus;
using EffectResult = gaudere::scheduling::wake::EffectResult;

constexpr std::size_t max_checkpoint_bytes = 32 * 1024;

struct CognitionEvidence {
    Task task;
    CurrentCognitionTaskInspection linkage;
    CanonicalCognitionDecision decision;
    std::string result_sha256;
};

struct ProviderActionEvidence {
    Action action;
    std::string expected_id;
    std::string expected_key;
};

struct PulseContextEvidence {
    std::uint64_t provider_total_before = 0;
    std::string provider_scope;
    Json historical_wake;
};

bool same_limits(const gaudere::work::ResourceLimits& a,
                 const gaudere::work::ResourceLimits& b) noexcept
{
    return a.max_input_bytes == b.max_input_bytes
        && a.max_output_bytes == b.max_output_bytes
        && a.max_runtime == b.max_runtime
        && a.max_attempts == b.max_attempts;
}

bool same_definition(const Task& a, const Task& b) noexcept
{
    return a.id == b.id && a.idempotency_key == b.idempotency_key
        && a.kind == b.kind && a.input_content_type == b.input_content_type
        && a.input == b.input && same_limits(a.limits, b.limits);
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
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

Json wake_summary(gaudere::scheduling::wake::WakeIntentStore& wake_store)
{
    const auto inspection = wake_store.inspect_scope(bounded_reflection_wake_scope);
    using Result = gaudere::scheduling::wake::WakeIntentScopeResult;
    switch (inspection.result) {
    case Result::empty:
        return Json{{"scope", bounded_reflection_wake_scope},
                    {"cardinality", "empty"}};
    case Result::ambiguous:
        throw std::runtime_error("historical wake scope is ambiguous");
    case Result::one:
        if (!inspection.intent)
            throw std::runtime_error("wake scope reported one row without intent");
        return Json{
            {"scope", bounded_reflection_wake_scope},
            {"cardinality", "one"},
            {"id", inspection.intent->id},
            {"source_id", inspection.intent->source_id},
            {"status", wake_status_name(inspection.intent->status)},
            {"accepted_at_ms", milliseconds(inspection.intent->accepted_at)},
            {"due_at_ms", milliseconds(inspection.intent->due_at)}
        };
    }
    throw std::runtime_error("unknown wake scope inspection result");
}

CognitionEvidence inspect_succeeded_current(
    gaudere::work::TaskStore& task_store,
    const std::string& task_id)
{
    const auto found = task_store.find(task_id);
    if (!found)
        throw std::invalid_argument("current cognition Task is missing: " + task_id);
    if (found->kind != current_cognition_task_kind
        || !valid_current_cognition_task(*found)) {
        throw std::invalid_argument("current cognition Task is non-canonical: " + task_id);
    }
    if (found->status != TaskStatus::succeeded || !found->result
        || found->result->content_type != resume_after_wake_decision_content_type) {
        throw std::invalid_argument("current cognition Task is not canonically succeeded: " + task_id);
    }
    const auto decision = inspect_canonical_cognition_decision(found->result->output);
    if (!decision.eligible)
        throw std::invalid_argument("current cognition result is non-canonical: " + decision.detail);
    const auto linkage = inspect_current_cognition_task(*found);
    if (!linkage.eligible)
        throw std::invalid_argument("current cognition linkage is non-canonical: " + linkage.detail);
    return {*found, linkage, decision, sha256_hex(found->result->output)};
}

ProviderActionEvidence inspect_confirmed_provider_action(
    gaudere::scheduling::wake::ActionStore& action_store,
    const Task& task)
{
    const std::string prefix = "provider.call:openai.responses:";
    const auto expected_id = prefix + task.id;
    const auto expected_key = prefix + task.idempotency_key;
    const auto by_id = action_store.find(expected_id);
    const auto by_key = action_store.find_by_idempotency_key(expected_key);
    if (!by_id || !by_key || by_id->id != by_key->id
        || by_id->id != expected_id
        || by_id->idempotency_key != expected_key) {
        throw std::invalid_argument(
            "expected provider Action identity is missing or ambiguous for " + task.id);
    }
    if (by_id->status != ActionStatus::succeeded
        || by_id->effect_result != EffectResult::confirmed
        || by_id->lease.has_value()) {
        throw std::invalid_argument(
            "expected provider Action is not terminal confirmed for " + task.id);
    }
    return {*by_id, expected_id, expected_key};
}

Json action_json(const ProviderActionEvidence& evidence)
{
    return Json{
        {"id", evidence.expected_id},
        {"idempotency_key", evidence.expected_key},
        {"status", "succeeded"},
        {"effect", "confirmed"},
        {"critical", evidence.action.critical}
    };
}

PulseContextEvidence inspect_pulse_context(
    const CognitionEvidence& audited,
    const CognitionEvidence& predecessor)
{
    Json capsule;
    Json facts;
    try {
        capsule = Json::parse(audited.linkage.snapshot_capsule);
        if (!capsule.is_object() || !capsule.contains("content")
            || !capsule.at("content").is_string()) {
            throw std::invalid_argument("snapshot capsule does not expose textual content");
        }
        facts = Json::parse(capsule.at("content").get<std::string>());
    } catch (const std::invalid_argument&) {
        throw;
    } catch (...) {
        throw std::invalid_argument("autonomous pulse context is not valid JSON");
    }
    if (!facts.is_object() || facts.value("schema", "")
            != "gaudere.autonomous-pulse-context.v0"
        || !facts.contains("predecessor") || !facts.at("predecessor").is_object()
        || !facts.contains("provider_budget") || !facts.at("provider_budget").is_object()
        || !facts.contains("historical_wake") || !facts.at("historical_wake").is_object()) {
        throw std::invalid_argument("audited cognition does not contain canonical autonomous pulse facts");
    }

    const auto& predecessor_fact = facts.at("predecessor");
    if (!predecessor_fact.contains("task_id")
        || !predecessor_fact.at("task_id").is_string()
        || !predecessor_fact.contains("result_sha256")
        || !predecessor_fact.at("result_sha256").is_string()
        || !predecessor_fact.contains("decision")
        || !predecessor_fact.at("decision").is_object()
        || predecessor_fact.at("task_id").get<std::string>() != predecessor.task.id
        || predecessor_fact.at("result_sha256").get<std::string>()
            != predecessor.result_sha256
        || predecessor_fact.at("decision").dump()
            != predecessor.decision.canonical_output) {
        throw std::invalid_argument("autonomous pulse predecessor evidence disagrees with durable Task");
    }
    if (audited.linkage.predecessor_task_id != predecessor.task.id
        || audited.linkage.predecessor_decision
            != predecessor.decision.canonical_output) {
        throw std::invalid_argument("audited cognition linkage disagrees with predecessor Task");
    }

    const auto& budget = facts.at("provider_budget");
    if (!budget.contains("scope") || !budget.at("scope").is_string()
        || budget.at("scope").get<std::string>() != openai_budget_scope()
        || !budget.contains("total_used")
        || !(budget.at("total_used").is_number_unsigned()
             || budget.at("total_used").is_number_integer())) {
        throw std::invalid_argument("autonomous pulse provider budget evidence is invalid");
    }
    const auto total = budget.at("total_used").get<std::int64_t>();
    if (total < 0)
        throw std::invalid_argument("autonomous pulse provider total is negative");
    return {static_cast<std::uint64_t>(total),
            budget.at("scope").get<std::string>(),
            facts.at("historical_wake")};
}

Task make_checkpoint_task(const std::string& canonical,
                          const std::string& audited_task_id)
{
    Task task;
    task.id = std::string{continuity_delta_checkpoint_task_prefix}
        + sha256_hex(canonical);
    task.idempotency_key = std::string{continuity_delta_checkpoint_task_prefix}
        + "audited:" + audited_task_id;
    task.kind = continuity_delta_checkpoint_task_kind;
    task.input_content_type = continuity_delta_checkpoint_content_type;
    task.input = canonical;
    task.limits.max_input_bytes = max_checkpoint_bytes;
    task.limits.max_output_bytes = max_checkpoint_bytes;
    task.limits.max_runtime = std::chrono::seconds{2};
    task.limits.max_attempts = 2;
    return task;
}

bool canonical_checkpoint_task(const Task& task) noexcept
{
    try {
        if (task.kind != continuity_delta_checkpoint_task_kind
            || task.input_content_type != continuity_delta_checkpoint_content_type
            || task.input.empty() || task.input.size() > max_checkpoint_bytes)
            return false;
        const auto parsed = Json::parse(task.input);
        if (!parsed.is_object()
            || parsed.value("schema", "") != continuity_delta_checkpoint_schema
            || !parsed.contains("audited") || !parsed.at("audited").is_object()
            || !parsed.at("audited").contains("task_id")
            || !parsed.at("audited").at("task_id").is_string()
            || parsed.dump() != task.input) return false;
        const auto expected = make_checkpoint_task(
            task.input, parsed.at("audited").at("task_id").get<std::string>());
        return same_definition(task, expected);
    } catch (...) {
        return false;
    }
}

class CheckpointIdentityHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        if (context.cancellation_requested())
            return {HandlerOutcome::cancelled, {}, {}, {}, {}};
        if (!canonical_checkpoint_task(context.task)) {
            return {HandlerOutcome::failed, {}, {},
                    "invalid_continuity_delta_checkpoint",
                    "checkpoint Task is not canonical"};
        }
        return {HandlerOutcome::succeeded,
                continuity_delta_checkpoint_content_type,
                context.task.input, {}, {}};
    }
};

bool canonical_success(const Task& task) noexcept
{
    return task.status == TaskStatus::succeeded && task.result
        && canonical_checkpoint_task(task)
        && task.result->content_type == continuity_delta_checkpoint_content_type
        && task.result->output == task.input
        && task.result->failure_code.empty()
        && task.result->failure_message.empty();
}

ContinuityDeltaCheckpointRecord complete_task(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& store,
    const Task& task,
    const ContinuityDeltaCheckpointResult success_result)
{
    CheckpointIdentityHandler handler;
    TaskExecutor executor(runtime, store);
    if (executor.execute(task.id, "continuity-delta-checkpoint", handler)
        != ExecuteResult::completed) {
        return {ContinuityDeltaCheckpointResult::unavailable,
                store.find(task.id),
                "checkpoint Task is not currently startable"};
    }
    const auto completed = store.find(task.id);
    if (!completed || !canonical_success(*completed)) {
        return {ContinuityDeltaCheckpointResult::conflict, completed,
                "checkpoint Task did not complete canonically"};
    }
    return {success_result, completed, {}};
}

} // namespace

ContinuityDeltaCheckpoint::ContinuityDeltaCheckpoint(
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::ActionStore& action_store,
    gaudere::budget::Store& budget_store,
    gaudere::scheduling::wake::WakeIntentStore& wake_store,
    gaudere::work::Runtime& work_runtime,
    Now now,
    PhaseHook phase_hook)
    : task_store_(task_store), action_store_(action_store),
      budget_store_(budget_store), wake_store_(wake_store),
      work_runtime_(work_runtime), now_(std::move(now)),
      phase_hook_(std::move(phase_hook))
{
    if (!now_) throw std::invalid_argument("continuity checkpoint clock is required");
}

ContinuityDeltaCheckpointRecord ContinuityDeltaCheckpoint::checkpoint(
    const std::string& audited_task_id)
{
    if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
        return {ContinuityDeltaCheckpointResult::unavailable, {},
                "work runtime is not running"};
    }
    try {
        const auto audited = inspect_succeeded_current(task_store_, audited_task_id);
        const auto predecessor = inspect_succeeded_current(
            task_store_, audited.linkage.predecessor_task_id);
        const auto predecessor_action = inspect_confirmed_provider_action(
            action_store_, predecessor.task);
        const auto audited_action = inspect_confirmed_provider_action(
            action_store_, audited.task);
        const auto pulse_context = inspect_pulse_context(audited, predecessor);
        const auto current_budget = budget_store_.snapshot(
            std::string{openai_budget_scope()}, now_(),
            openai_bootstrap_budget_policy());
        if (current_budget.total_used != pulse_context.provider_total_before + 1U) {
            return {ContinuityDeltaCheckpointResult::conflict, {},
                    "current provider total does not equal audited context total plus one"};
        }

        const auto current_wake = wake_summary(wake_store_);
        if (current_wake != pulse_context.historical_wake) {
            return {ContinuityDeltaCheckpointResult::conflict, {},
                    "historical WakeIntent evidence changed since audited context"};
        }

        const Json payload = {
            {"schema", continuity_delta_checkpoint_schema},
            {"audited", Json{
                {"task_id", audited.task.id},
                {"result_sha256", audited.result_sha256},
                {"decision", Json::parse(audited.decision.canonical_output)},
                {"provider_action", action_json(audited_action)}
            }},
            {"predecessor", Json{
                {"task_id", predecessor.task.id},
                {"result_sha256", predecessor.result_sha256},
                {"decision", Json::parse(predecessor.decision.canonical_output)},
                {"provider_action", action_json(predecessor_action)}
            }},
            {"audited_context", Json{
                {"snapshot_task_id", audited.linkage.snapshot_task_id},
                {"captured_at_ms", audited.linkage.captured_at_ms},
                {"provider_budget_scope", pulse_context.provider_scope},
                {"provider_total_before", pulse_context.provider_total_before},
                {"historical_wake", pulse_context.historical_wake}
            }},
            {"current_provider_budget", Json{
                {"scope", std::string{openai_budget_scope()}},
                {"total_used", current_budget.total_used}
            }},
            {"reconciliation", Json{
                {"provider_increment_from_audited_context", 1},
                {"predecessor_provider_effect_confirmed", true},
                {"audited_provider_effect_confirmed", true},
                {"statement", "The durable audited context already includes the confirmed predecessor provider effect; older provider totals that omit that effect remain historical evidence and are superseded for current accounting."}
            }},
            {"unresolved_external", Json::array({
                "external_checkpoint_identity",
                "rollback_reference",
                "stopped_state_backup_marker"
            })}
        };
        const auto canonical = payload.dump();
        if (canonical.size() > max_checkpoint_bytes) {
            return {ContinuityDeltaCheckpointResult::conflict, {},
                    "canonical checkpoint exceeds its bound"};
        }
        const auto expected = make_checkpoint_task(canonical, audited.task.id);
        const auto by_id = task_store_.find(expected.id);
        const auto by_key = task_store_.find_by_idempotency_key(expected.idempotency_key);
        if (by_id || by_key) {
            if (by_id && by_key && by_id->id != by_key->id) {
                return {ContinuityDeltaCheckpointResult::conflict, {},
                        "checkpoint id and idempotency key resolve differently"};
            }
            const auto existing = by_id ? by_id : by_key;
            if (!existing || !same_definition(*existing, expected)) {
                return {ContinuityDeltaCheckpointResult::conflict, existing,
                        "existing audited checkpoint conflicts with current durable facts"};
            }
            if (gaudere::work::is_terminal(existing->status)) {
                if (!canonical_success(*existing)) {
                    return {ContinuityDeltaCheckpointResult::conflict, existing,
                            "terminal checkpoint lacks canonical successful result"};
                }
                return {ContinuityDeltaCheckpointResult::duplicate, existing, {}};
            }
            return complete_task(work_runtime_, task_store_, *existing,
                                 ContinuityDeltaCheckpointResult::duplicate);
        }

        switch (work_runtime_.submit(expected)) {
        case gaudere::work::SubmitResult::accepted:
            break;
        case gaudere::work::SubmitResult::duplicate:
            return {ContinuityDeltaCheckpointResult::conflict, {},
                    "checkpoint submission became duplicate without durable match"};
        case gaudere::work::SubmitResult::invalid:
            return {ContinuityDeltaCheckpointResult::conflict, {},
                    "canonical checkpoint Task was rejected as invalid"};
        case gaudere::work::SubmitResult::unavailable:
            return {ContinuityDeltaCheckpointResult::unavailable, {},
                    "work runtime rejected checkpoint submission"};
        }
        if (phase_hook_) phase_hook_("after_submit");
        return complete_task(work_runtime_, task_store_, expected,
                             ContinuityDeltaCheckpointResult::accepted);
    } catch (const std::invalid_argument& error) {
        return {ContinuityDeltaCheckpointResult::ineligible, {}, error.what()};
    } catch (const std::exception& error) {
        return {ContinuityDeltaCheckpointResult::unavailable, {}, error.what()};
    }
}

} // namespace gaudere_agent
