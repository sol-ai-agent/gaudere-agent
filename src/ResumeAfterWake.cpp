#include "ResumeAfterWake.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using WakeIntent = gaudere::scheduling::wake::WakeIntent;
using WakeIntentScopeResult = gaudere::scheduling::wake::WakeIntentScopeResult;
using WakeIntentStatus = gaudere::scheduling::wake::WakeIntentStatus;

struct Eligibility {
    bool eligible = false;
    bool not_found = false;
    std::optional<WakeIntent> wake;
    std::optional<Task> source;
    std::string detail;
};

std::int64_t milliseconds(
    const gaudere::scheduling::wake::WakeIntentTimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string quoted(const std::string& value)
{
    return Json(value).dump();
}

const char* task_status_name(const TaskStatus status) noexcept
{
    switch (status) {
    case TaskStatus::pending: return "pending";
    case TaskStatus::running: return "running";
    case TaskStatus::cancel_requested: return "cancel_requested";
    case TaskStatus::succeeded: return "succeeded";
    case TaskStatus::failed: return "failed";
    case TaskStatus::cancelled: return "cancelled";
    case TaskStatus::manual_review: return "manual_review";
    }
    return "unknown";
}

const char* state_name(const ResumeAfterWakeState state) noexcept
{
    switch (state) {
    case ResumeAfterWakeState::disabled: return "disabled";
    case ResumeAfterWakeState::ineligible: return "ineligible";
    case ResumeAfterWakeState::eligible: return "eligible";
    case ResumeAfterWakeState::claimed: return "claimed";
    case ResumeAfterWakeState::completed: return "completed";
    case ResumeAfterWakeState::failed: return "failed";
    case ResumeAfterWakeState::manual_review: return "manual_review";
    }
    return "ineligible";
}

bool same_limits(const gaudere::work::ResourceLimits& left,
                 const gaudere::work::ResourceLimits& right) noexcept
{
    return left.max_input_bytes == right.max_input_bytes
        && left.max_output_bytes == right.max_output_bytes
        && left.max_runtime == right.max_runtime
        && left.max_attempts == right.max_attempts;
}

bool same_definition(const Task& existing, const Task& expected) noexcept
{
    return existing.id == expected.id
        && existing.idempotency_key == expected.idempotency_key
        && existing.kind == expected.kind
        && existing.input_content_type == expected.input_content_type
        && existing.input == expected.input
        && same_limits(existing.limits, expected.limits);
}

Json canonical_source_json(const Task& source)
{
    if (!source.result) {
        throw std::invalid_argument("resume source result is missing");
    }
    try {
        return Json::parse(source.result->output);
    } catch (...) {
        throw std::invalid_argument("resume source result is not valid JSON");
    }
}

Task make_resume_task(const Task& source, const WakeIntent& wake)
{
    if (!wake.terminal_at) {
        throw std::invalid_argument("resume wake lacks terminal timestamp");
    }

    Json context = {
        {"schema", resume_after_wake_context_schema},
        {"source_task_id", source.id},
        {"source_decision", canonical_source_json(source)},
        {"wake", {
            {"id", wake.id},
            {"accepted_at_ms", milliseconds(wake.accepted_at)},
            {"due_at_ms", milliseconds(wake.due_at)},
            {"terminal_at_ms", milliseconds(*wake.terminal_at)},
            {"terminal_reason", wake.terminal_reason}
        }}
    };

    const std::string prompt =
        "You are Gaudere's bounded resume-after-wake cognition v0.\n"
        "Continue only from the durable context supplied below. Do not claim "
        "memory or observations that are absent from it.\n"
        "Return exactly one JSON object and no markdown or surrounding text.\n"
        "Use exactly one of these forms:\n"
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"stop\",\"reason\":\"...\"}\n"
        "{\"schema\":\"gaudere.cognition.resume-decision.v1\","
        "\"decision\":\"continue\",\"reason\":\"...\","
        "\"objective\":\"...\"}\n"
        "Do not add keys. reason must be non-empty and at most 1024 UTF-8 bytes. "
        "objective is required only for continue, must be non-empty, and is at "
        "most 4096 UTF-8 bytes. This result is a proposal only and grants no "
        "authority to execute tools, shell commands, network calls, or external "
        "actions.\n"
        "Durable resume context:\n" + context.dump();

    Task task;
    task.id = std::string{resume_after_wake_task_prefix} + wake.id;
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = prompt;
    task.limits.max_input_bytes = 16 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = std::chrono::seconds{60};
    // Attempt two is reconciliation only. Existing provider Action evidence must
    // prevent a second provider invocation in any future provider-bearing slice.
    task.limits.max_attempts = 2;
    if (task.input.size() > task.limits.max_input_bytes) {
        throw std::invalid_argument(
            "resume-after-wake prompt exceeds hard input limit");
    }
    return task;
}

Eligibility evaluate(gaudere::work::TaskStore& task_store,
                     gaudere::scheduling::wake::WakeIntentStore& wake_store,
                     const std::string& wake_id)
{
    gaudere::scheduling::wake::WakeIntentScopeInspection scoped;
    try {
        scoped = wake_store.inspect_scope(bounded_reflection_wake_scope);
    } catch (...) {
        return {false, false, {}, {}, "wake scope inspection is unavailable"};
    }

    if (scoped.result == WakeIntentScopeResult::empty) {
        return {false, true, {}, {}, "wake not found"};
    }
    if (scoped.result == WakeIntentScopeResult::ambiguous || !scoped.intent) {
        return {false, false, {}, {}, "wake scope is ambiguous"};
    }

    const auto wake = *scoped.intent;
    if (wake.id != wake_id) {
        return {false, true, wake, {}, "wake not found"};
    }
    if (wake.scope != bounded_reflection_wake_scope
        || wake.source_id != wake.id) {
        return {false, false, wake, {},
                "wake identity/source relationship is invalid"};
    }
    if (!gaudere::scheduling::wake::valid_wake_intent(wake)) {
        return {false, false, wake, {}, "wake durable shape is invalid"};
    }
    if (wake.status != WakeIntentStatus::fired || !wake.terminal_at
        || *wake.terminal_at < wake.due_at) {
        return {false, false, wake, {},
                "wake is not a valid fired terminal intent"};
    }

    const auto source = task_store.find(wake.source_id);
    if (!source) {
        return {false, false, wake, {}, "source task is missing"};
    }
    const auto decision = inspect_wake_source_decision(*source);
    if (!decision.eligible) {
        return {false, false, wake, source, decision.detail};
    }

    const auto durable_delay =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            wake.due_at - wake.accepted_at);
    const auto source_delay =
        std::chrono::duration_cast<std::chrono::milliseconds>(decision.delay);
    if (durable_delay != source_delay) {
        return {false, false, wake, source,
                "wake deadline does not match canonical source proposal"};
    }

    return {true, false, wake, source, {}};
}

struct ExistingClaim {
    bool exists = false;
    bool conflict = false;
    std::optional<Task> task;
    std::string detail;
};

ExistingClaim inspect_existing(gaudere::work::TaskStore& task_store,
                               const Task& expected)
{
    const auto by_id = task_store.find(expected.id);
    const auto by_key =
        task_store.find_by_idempotency_key(expected.idempotency_key);
    if (!by_id && !by_key) {
        return {};
    }
    if (by_id && by_key && by_id->id != by_key->id) {
        return {true, true, {},
                "resume Task id and idempotency key resolve to different Tasks"};
    }
    const auto candidate = by_id ? by_id : by_key;
    if (!candidate || candidate->id != expected.id
        || candidate->idempotency_key != expected.idempotency_key
        || !same_definition(*candidate, expected)) {
        return {true, true, candidate,
                "existing resume Task conflicts with canonical definition"};
    }
    return {true, false, candidate, {}};
}

ResumeAfterWakeState lifecycle_state(const TaskStatus status) noexcept
{
    switch (status) {
    case TaskStatus::pending:
    case TaskStatus::running:
    case TaskStatus::cancel_requested:
        return ResumeAfterWakeState::claimed;
    case TaskStatus::succeeded:
        return ResumeAfterWakeState::completed;
    case TaskStatus::failed:
    case TaskStatus::cancelled:
        return ResumeAfterWakeState::failed;
    case TaskStatus::manual_review:
        return ResumeAfterWakeState::manual_review;
    }
    return ResumeAfterWakeState::manual_review;
}

std::string status_report(const ResumeAfterWakeState state,
                          const bool enabled,
                          const std::string& wake_id,
                          const std::optional<Task>& task,
                          const std::string& detail,
                          const bool healthy)
{
    std::ostringstream output;
    output << "report_schema=\"gaudere.resume_after_wake_status.v1\"\n"
           << "enabled=" << (enabled ? "true" : "false") << '\n'
           << "wake_id=" << quoted(wake_id) << '\n'
           << "state=" << state_name(state) << '\n';
    if (task) {
        output << "resume_task_id=" << quoted(task->id) << '\n'
               << "resume_task_idempotency_key="
               << quoted(task->idempotency_key) << '\n'
               << "resume_task_kind=" << quoted(task->kind) << '\n'
               << "resume_task_status=" << task_status_name(task->status) << '\n'
               << "resume_task_attempts=" << task->attempts_started << '/'
               << task->limits.max_attempts << '\n';
    } else {
        output << "resume_task_id="
               << quoted(std::string{resume_after_wake_task_prefix} + wake_id)
               << '\n'
               << "resume_task_status=none\n";
    }
    output << "detail=" << quoted(detail) << '\n'
           << "health=" << (healthy ? "ok" : "attention") << '\n';
    return output.str();
}

} // namespace

ResumeAfterWake::ResumeAfterWake(
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::WakeIntentStore& wake_store,
    gaudere::work::Runtime& work_runtime,
    const bool enabled) noexcept
    : task_store_(task_store),
      wake_store_(wake_store),
      work_runtime_(work_runtime),
      enabled_(enabled)
{
}

ResumeAfterWakeClaim ResumeAfterWake::claim(const std::string& wake_id)
{
    if (!enabled_) {
        return {ResumeAfterWakeClaimResult::disabled, {},
                "resume-after-wake capability is disabled"};
    }

    const auto eligibility = evaluate(task_store_, wake_store_, wake_id);
    if (!eligibility.eligible) {
        return {eligibility.not_found
                    ? ResumeAfterWakeClaimResult::wake_not_found
                    : ResumeAfterWakeClaimResult::ineligible,
                {}, eligibility.detail};
    }

    Task expected;
    try {
        expected = make_resume_task(*eligibility.source, *eligibility.wake);
    } catch (const std::exception& error) {
        return {ResumeAfterWakeClaimResult::ineligible, {}, error.what()};
    }

    const auto existing = inspect_existing(task_store_, expected);
    if (existing.conflict) {
        return {ResumeAfterWakeClaimResult::conflict,
                existing.task, existing.detail};
    }
    if (existing.exists) {
        return {ResumeAfterWakeClaimResult::duplicate, existing.task, {}};
    }

    switch (work_runtime_.submit(expected)) {
    case gaudere::work::SubmitResult::accepted: {
        const auto stored = task_store_.find(expected.id);
        if (!stored || !same_definition(*stored, expected)) {
            throw std::runtime_error(
                "accepted resume Task is missing or differs from canonical definition");
        }
        return {ResumeAfterWakeClaimResult::accepted, stored, {}};
    }
    case gaudere::work::SubmitResult::duplicate: {
        const auto raced = inspect_existing(task_store_, expected);
        if (!raced.exists || raced.conflict || !raced.task) {
            return {ResumeAfterWakeClaimResult::conflict, raced.task,
                    raced.detail.empty()
                        ? "duplicate resume submission lacks canonical durable Task"
                        : raced.detail};
        }
        return {ResumeAfterWakeClaimResult::duplicate, raced.task, {}};
    }
    case gaudere::work::SubmitResult::invalid:
        return {ResumeAfterWakeClaimResult::conflict, {},
                "canonical resume Task was rejected as invalid"};
    case gaudere::work::SubmitResult::unavailable:
        return {ResumeAfterWakeClaimResult::unavailable, {},
                "work runtime is unavailable"};
    }
    throw std::logic_error("unknown work submit result");
}

ResumeAfterWakeStatus ResumeAfterWake::inspect(const std::string& wake_id) const
{
    if (!enabled_) {
        return {ResumeAfterWakeState::disabled, true,
                status_report(ResumeAfterWakeState::disabled, false, wake_id,
                              {}, "resume-after-wake capability is disabled", true)};
    }

    const auto eligibility = evaluate(task_store_, wake_store_, wake_id);
    if (!eligibility.eligible) {
        return {ResumeAfterWakeState::ineligible, !eligibility.not_found,
                status_report(ResumeAfterWakeState::ineligible, true, wake_id,
                              {}, eligibility.detail, !eligibility.not_found)};
    }

    Task expected;
    try {
        expected = make_resume_task(*eligibility.source, *eligibility.wake);
    } catch (const std::exception& error) {
        return {ResumeAfterWakeState::ineligible, false,
                status_report(ResumeAfterWakeState::ineligible, true, wake_id,
                              {}, error.what(), false)};
    }

    const auto existing = inspect_existing(task_store_, expected);
    if (existing.conflict) {
        return {ResumeAfterWakeState::manual_review, false,
                status_report(ResumeAfterWakeState::manual_review, true, wake_id,
                              existing.task, existing.detail, false)};
    }
    if (!existing.exists || !existing.task) {
        return {ResumeAfterWakeState::eligible, true,
                status_report(ResumeAfterWakeState::eligible, true, wake_id,
                              {}, {}, true)};
    }

    const auto state = lifecycle_state(existing.task->status);
    const bool healthy = state != ResumeAfterWakeState::manual_review;
    return {state, healthy,
            status_report(state, true, wake_id, existing.task, {}, healthy)};
}

} // namespace gaudere_agent
