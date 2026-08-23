#include "ExplicitWake.hpp"

#include "BoundedReflection.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using WakeIntent = gaudere::scheduling::wake::WakeIntent;
using WakeIntentAcceptResult =
    gaudere::scheduling::wake::WakeIntentAcceptResult;
using WakeIntentScopeResult =
    gaudere::scheduling::wake::WakeIntentScopeResult;
using WakeIntentStatus = gaudere::scheduling::wake::WakeIntentStatus;
using WakeIntentTimePoint =
    gaudere::scheduling::wake::WakeIntentTimePoint;

constexpr std::size_t max_decision_bytes = 4096;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::uint64_t min_wake_after_seconds = 900;
constexpr std::uint64_t max_wake_after_seconds = 86400;

struct SourceDecision {
    bool eligible = false;
    std::chrono::seconds delay{0};
    std::string detail;
};

bool unsigned_integer(const Json& value, std::uint64_t& output) noexcept
{
    try {
        if (value.is_number_unsigned()) {
            output = value.get<std::uint64_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const auto signed_value = value.get<std::int64_t>();
            if (signed_value < 0) {
                return false;
            }
            output = static_cast<std::uint64_t>(signed_value);
            return true;
        }
    } catch (...) {
    }
    return false;
}

SourceDecision source_decision(const gaudere::work::Task& task)
{
    if (task.kind != bounded_reflection_task_kind) {
        return {false, {}, "source task kind is not cognition.reflect.v1"};
    }
    if (task.status != gaudere::work::TaskStatus::succeeded) {
        return {false, {}, "source task is not succeeded"};
    }
    if (!task.result) {
        return {false, {}, "source task result is missing"};
    }
    if (task.result->content_type != bounded_reflection_decision_content_type) {
        return {false, {},
                "source task result is not a cognition decision"};
    }
    if (task.result->output.empty()
        || task.result->output.size() > max_decision_bytes) {
        return {false, {}, "source task decision exceeds its durable bounds"};
    }

    Json decision;
    try {
        decision = Json::parse(task.result->output);
    } catch (...) {
        return {false, {}, "source task decision is not valid JSON"};
    }
    if (!decision.is_object() || decision.size() != 4
        || !decision.contains("schema") || !decision.at("schema").is_string()
        || decision.at("schema").get<std::string>()
            != "gaudere.cognition.decision.v1"
        || !decision.contains("decision")
        || !decision.at("decision").is_string()
        || decision.at("decision").get<std::string>() != "propose_wake"
        || !decision.contains("reason") || !decision.at("reason").is_string()
        || !decision.contains("wake_after_seconds")) {
        return {false, {},
                "source task decision is not a propose_wake object"};
    }

    const auto reason = decision.at("reason").get<std::string>();
    std::uint64_t wake_after_seconds = 0;
    if (reason.empty() || reason.size() > max_reason_bytes
        || !unsigned_integer(decision.at("wake_after_seconds"),
                             wake_after_seconds)
        || wake_after_seconds < min_wake_after_seconds
        || wake_after_seconds > max_wake_after_seconds) {
        return {false, {}, "source task wake proposal is outside hard bounds"};
    }

    const Json canonical = {
        {"schema", "gaudere.cognition.decision.v1"},
        {"decision", "propose_wake"},
        {"reason", reason},
        {"wake_after_seconds", wake_after_seconds}
    };
    if (canonical.dump() != task.result->output) {
        return {false, {}, "source task wake proposal is not canonical"};
    }

    return {true,
            std::chrono::seconds{static_cast<std::int64_t>(wake_after_seconds)},
            {}};
}

std::string status_name(const WakeIntentStatus status)
{
    switch (status) {
    case WakeIntentStatus::scheduled:
        return "scheduled";
    case WakeIntentStatus::fired:
        return "fired";
    case WakeIntentStatus::revoked:
        return "revoked";
    case WakeIntentStatus::manual_review:
        return "manual_review";
    }
    throw std::invalid_argument("unknown wake-intent status");
}

const char* task_status_name(const gaudere::work::TaskStatus status) noexcept
{
    using Status = gaudere::work::TaskStatus;
    switch (status) {
    case Status::pending: return "pending";
    case Status::running: return "running";
    case Status::cancel_requested: return "cancel_requested";
    case Status::succeeded: return "succeeded";
    case Status::failed: return "failed";
    case Status::cancelled: return "cancelled";
    case Status::manual_review: return "manual_review";
    }
    return "unknown";
}

std::int64_t milliseconds(const WakeIntentTimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string quoted(const std::string& value)
{
    return Json(value).dump();
}

void time_line(std::ostringstream& output,
               const char* name,
               const std::optional<WakeIntentTimePoint>& value)
{
    output << name << '=';
    if (value) {
        output << milliseconds(*value);
    } else {
        output << "none";
    }
    output << '\n';
}

bool safe_revocation_reason(const std::string& reason) noexcept
{
    if (reason.empty() || reason.size() > max_reason_bytes) {
        return false;
    }
    for (const unsigned char character : reason) {
        if (character < 0x20 || character == 0x7f) {
            return false;
        }
    }
    try {
        static_cast<void>(Json(reason).dump());
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

ExplicitWake::ExplicitWake(
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::WakeIntentRuntime& wake_runtime)
    : task_store_(task_store),
      wake_runtime_(wake_runtime)
{
    if (wake_runtime_.scope() != explicit_wake_scope
        || wake_runtime_.policy().max_total != explicit_wake_max_total) {
        throw std::invalid_argument(
            "explicit wake runtime does not match the fixed v0 capability");
    }
}

ExplicitWakeAcceptance ExplicitWake::accept(
    const std::string& source_task_id)
{
    const auto task = task_store_.find(source_task_id);
    if (!task) {
        return {ExplicitWakeAcceptResult::source_not_found, {},
                "source task not found"};
    }
    const auto source = source_decision(*task);
    if (!source.eligible) {
        return {ExplicitWakeAcceptResult::source_ineligible, {}, source.detail};
    }

    const auto result = wake_runtime_.accept(
        task->id, task->id,
        std::chrono::duration_cast<std::chrono::milliseconds>(source.delay));
    switch (result) {
    case WakeIntentAcceptResult::accepted:
    case WakeIntentAcceptResult::duplicate: {
        const auto stored = wake_runtime_.find(task->id);
        if (!stored) {
            throw std::runtime_error(
                "accepted explicit wake is missing from durable state");
        }
        return {result == WakeIntentAcceptResult::accepted
                    ? ExplicitWakeAcceptResult::accepted
                    : ExplicitWakeAcceptResult::duplicate,
                stored, {}};
    }
    case WakeIntentAcceptResult::total_exhausted:
        return {ExplicitWakeAcceptResult::total_exhausted, {},
                "explicit wake lifetime limit is exhausted"};
    case WakeIntentAcceptResult::conflict:
        return {ExplicitWakeAcceptResult::conflict, {},
                "explicit wake identity conflicts with durable state"};
    case WakeIntentAcceptResult::invalid:
        return {ExplicitWakeAcceptResult::invalid, {},
                "explicit wake deadline is invalid"};
    }
    throw std::logic_error("unknown wake-intent acceptance result");
}

gaudere::scheduling::wake::WakeIntentRevokeResult ExplicitWake::revoke(
    const std::string& wake_id,
    const std::string& reason)
{
    if (!safe_revocation_reason(reason)) {
        return gaudere::scheduling::wake::WakeIntentRevokeResult::invalid;
    }
    return wake_runtime_.revoke(wake_id, reason);
}

std::optional<WakeIntent> ExplicitWake::find(const std::string& wake_id) const
{
    return wake_runtime_.find(wake_id);
}

ExplicitWakeStatus ExplicitWake::inspect_status(
    std::optional<WakeIntentTimePoint> next_lease_at,
    std::optional<WakeIntentTimePoint> scheduler_next_at) const
{
    const auto scoped = wake_runtime_.inspect_scope();
    std::ostringstream output;
    output << "report_schema=\"gaudere.wake_status.v1\"\n"
           << "scope=" << quoted(wake_runtime_.scope()) << '\n';

    if (scoped.result == WakeIntentScopeResult::empty) {
        output << "record=none\n"
               << "health=empty\n"
               << "scheduler_coverage=not_applicable\n";
        return {true, output.str()};
    }
    if (scoped.result == WakeIntentScopeResult::ambiguous) {
        output << "record=ambiguous\n"
               << "health=ambiguous\n"
               << "scheduler_coverage=not_applicable\n";
        return {false, output.str()};
    }
    if (!scoped.intent) {
        throw std::runtime_error(
            "wake scope inspection reported one record without an intent");
    }

    const auto& intent = *scoped.intent;
    output << "record=one\n"
           << "id=" << quoted(intent.id) << '\n'
           << "source_task_id=" << quoted(intent.source_id) << '\n';

    const auto source_task = task_store_.find(intent.source_id);
    bool source_eligible = false;
    if (!source_task) {
        output << "source_consistency=missing\n";
    } else {
        const auto decision = source_decision(*source_task);
        source_eligible = decision.eligible;
        output << "source_task_kind=" << quoted(source_task->kind) << '\n'
               << "source_task_status=" << task_status_name(source_task->status) << '\n'
               << "source_task_attempts=" << source_task->attempts_started << '/'
               << source_task->limits.max_attempts << '\n'
               << "source_result_content_type="
               << quoted(source_task->result ? source_task->result->content_type
                                             : std::string{}) << '\n'
               << "source_consistency="
               << (source_eligible ? "eligible" : "ineligible") << '\n';
    }

    output << "status=" << status_name(intent.status) << '\n'
           << "accepted_at_ms=" << milliseconds(intent.accepted_at) << '\n'
           << "due_at_ms=" << milliseconds(intent.due_at) << '\n';
    if (intent.terminal_at) {
        output << "terminal_at_ms=" << milliseconds(*intent.terminal_at) << '\n';
    } else {
        output << "terminal_at_ms=none\n";
    }
    output << "terminal_reason=" << quoted(intent.terminal_reason) << '\n';

    const auto derived_wake_at = wake_runtime_.next_scheduled_at();
    time_line(output, "derived_wake_at_ms", derived_wake_at);
    time_line(output, "derived_lease_at_ms", next_lease_at);
    time_line(output, "scheduler_next_at_ms", scheduler_next_at);

    if (!source_task || !source_eligible) {
        output << "health=source_inconsistent\n"
               << "scheduler_coverage=not_applicable\n";
        return {false, output.str()};
    }

    if (intent.status == WakeIntentStatus::manual_review) {
        output << "health=manual_review\n"
               << "scheduler_coverage=not_applicable\n";
        return {false, output.str()};
    }
    if (intent.status == WakeIntentStatus::fired
        || intent.status == WakeIntentStatus::revoked) {
        output << "health=terminal\n"
               << "scheduler_coverage=not_applicable\n";
        return {true, output.str()};
    }

    if (!derived_wake_at || *derived_wake_at != intent.due_at) {
        output << "health=derived_mismatch\n"
               << "scheduler_coverage=derived_mismatch\n";
        return {false, output.str()};
    }
    if (!scheduler_next_at) {
        output << "health=scheduling_divergence\n"
               << "scheduler_coverage=missing\n";
        return {false, output.str()};
    }
    if (*scheduler_next_at > intent.due_at) {
        output << "health=scheduling_divergence\n"
               << "scheduler_coverage=late\n";
        return {false, output.str()};
    }
    if (*scheduler_next_at < intent.due_at) {
        output << "health=ok\n"
               << "scheduler_coverage=covered_by_earlier_event\n";
        return {true, output.str()};
    }

    output << "health=ok\n"
           << "scheduler_coverage="
           << (next_lease_at && *next_lease_at == intent.due_at
                   ? "shared_with_lease" : "exact")
           << '\n';
    return {true, output.str()};
}

std::string wake_intent_report(const WakeIntent& intent)
{
    std::ostringstream output;
    output << "scope=" << quoted(intent.scope) << '\n'
           << "id=" << quoted(intent.id) << '\n'
           << "source_id=" << quoted(intent.source_id) << '\n'
           << "status=" << status_name(intent.status) << '\n'
           << "accepted_at_ms=" << milliseconds(intent.accepted_at) << '\n'
           << "due_at_ms=" << milliseconds(intent.due_at) << '\n';
    if (intent.terminal_at) {
        output << "terminal_at_ms=" << milliseconds(*intent.terminal_at) << '\n';
    } else {
        output << "terminal_at_ms=none\n";
    }
    output << "terminal_reason=" << quoted(intent.terminal_reason) << '\n';
    return output.str();
}

} // namespace gaudere_agent
