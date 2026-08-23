#include "WakeSourceDecision.hpp"

#include "BoundedReflection.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_decision_bytes = 4096;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::uint64_t min_wake_after_seconds = 900;
constexpr std::uint64_t max_wake_after_seconds = 86400;

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

} // namespace

WakeSourceDecision inspect_wake_source_decision(const gaudere::work::Task& task)
{
    if (task.kind != bounded_reflection_task_kind) {
        return {false, {}, {}, {},
                "source task kind is not cognition.reflect.v1"};
    }
    if (task.status != gaudere::work::TaskStatus::succeeded) {
        return {false, {}, {}, {}, "source task is not succeeded"};
    }
    if (!task.result) {
        return {false, {}, {}, {}, "source task result is missing"};
    }
    if (task.result->content_type != bounded_reflection_decision_content_type) {
        return {false, {}, {}, {},
                "source task result is not a cognition decision"};
    }
    if (task.result->output.empty()
        || task.result->output.size() > max_decision_bytes) {
        return {false, {}, {}, {},
                "source task decision exceeds its durable bounds"};
    }

    Json decision;
    try {
        decision = Json::parse(task.result->output);
    } catch (...) {
        return {false, {}, {}, {}, "source task decision is not valid JSON"};
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
        return {false, {}, {}, {},
                "source task decision is not a propose_wake object"};
    }

    const auto reason = decision.at("reason").get<std::string>();
    std::uint64_t delay_seconds = 0;
    if (reason.empty() || reason.size() > max_reason_bytes
        || !unsigned_integer(decision.at("wake_after_seconds"), delay_seconds)
        || delay_seconds < min_wake_after_seconds
        || delay_seconds > max_wake_after_seconds) {
        return {false, {}, {}, {},
                "source task wake proposal is outside hard bounds"};
    }

    const Json canonical = {
        {"schema", "gaudere.cognition.decision.v1"},
        {"decision", "propose_wake"},
        {"reason", reason},
        {"wake_after_seconds", delay_seconds}
    };
    const auto canonical_output = canonical.dump();
    if (canonical_output != task.result->output) {
        return {false, {}, {}, {},
                "source task wake proposal is not canonical"};
    }

    return {true,
            std::chrono::seconds{static_cast<std::int64_t>(delay_seconds)},
            reason,
            canonical_output,
            {}};
}

} // namespace gaudere_agent
