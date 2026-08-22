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
using WakeIntentStatus = gaudere::scheduling::wake::WakeIntentStatus;

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
