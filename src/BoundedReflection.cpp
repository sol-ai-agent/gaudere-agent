#include "BoundedReflection.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_objective_bytes = 4096;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::uint64_t min_wake_after_seconds = 900;
constexpr std::uint64_t max_wake_after_seconds = 86400;

bool allowed_objective_controls(const std::string& value) noexcept
{
    for (const unsigned char character : value) {
        if (character < 0x20 && character != '\t'
            && character != '\n' && character != '\r') {
            return false;
        }
    }
    return true;
}

std::string reflection_prompt(const std::string& objective)
{
    if (objective.empty() || objective.size() > max_objective_bytes) {
        throw std::invalid_argument(
            "bounded reflection objective must be 1..4096 bytes");
    }
    if (!allowed_objective_controls(objective)) {
        throw std::invalid_argument(
            "bounded reflection objective contains a disallowed control byte");
    }

    std::string objective_document;
    try {
        objective_document = Json{{"objective", objective}}.dump();
    } catch (...) {
        throw std::invalid_argument(
            "bounded reflection objective must be valid UTF-8");
    }

    return
        "You are Gaudere's bounded reflection v0.\n"
        "Return exactly one JSON object and no markdown or surrounding text.\n"
        "Use exactly one of these forms:\n"
        "{\"schema\":\"gaudere.cognition.decision.v1\","
        "\"decision\":\"stop\",\"reason\":\"...\"}\n"
        "{\"schema\":\"gaudere.cognition.decision.v1\","
        "\"decision\":\"propose_wake\",\"reason\":\"...\","
        "\"wake_after_seconds\":900}\n"
        "Do not add keys. Keep reason non-empty and at most 1024 UTF-8 bytes.\n"
        "For propose_wake, wake_after_seconds must be an integer from 900 through 86400.\n"
        "The decision is a proposal only and grants no authority to act.\n"
        "Reflect only on the objective in this JSON object:\n"
        + objective_document;
}

HandlerResult invalid_decision(HandlerResult result, std::string message)
{
    return HandlerResult{HandlerOutcome::failed, {}, {},
                         "cognition_invalid_decision", std::move(message),
                         std::move(result.metadata_content_type),
                         std::move(result.metadata)};
}

bool only_known_keys(const Json& decision)
{
    for (auto entry = decision.begin(); entry != decision.end(); ++entry) {
        if (entry.key() != "schema" && entry.key() != "decision"
            && entry.key() != "reason"
            && entry.key() != "wake_after_seconds") {
            return false;
        }
    }
    return true;
}

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

HandlerResult normalize_decision(HandlerResult result)
{
    Json decision;
    try {
        decision = Json::parse(result.output);
    } catch (...) {
        return invalid_decision(
            std::move(result), "reflection output is not valid JSON");
    }

    if (!decision.is_object() || !only_known_keys(decision)
        || !decision.contains("schema") || !decision.at("schema").is_string()
        || decision.at("schema").get<std::string>()
            != "gaudere.cognition.decision.v1"
        || !decision.contains("decision")
        || !decision.at("decision").is_string()
        || !decision.contains("reason") || !decision.at("reason").is_string()) {
        return invalid_decision(
            std::move(result), "reflection output does not match the decision schema");
    }

    const auto action = decision.at("decision").get<std::string>();
    const auto reason = decision.at("reason").get<std::string>();
    if (reason.empty() || reason.size() > max_reason_bytes) {
        return invalid_decision(
            std::move(result), "reflection reason must be 1..1024 bytes");
    }

    Json normalized;
    if (action == "stop") {
        if (decision.size() != 3 || decision.contains("wake_after_seconds")) {
            return invalid_decision(
                std::move(result), "stop decision must not contain a wake delay");
        }
        normalized = Json{{"schema", "gaudere.cognition.decision.v1"},
                          {"decision", "stop"},
                          {"reason", reason}};
    } else if (action == "propose_wake") {
        if (decision.size() != 4 || !decision.contains("wake_after_seconds")) {
            return invalid_decision(
                std::move(result), "propose_wake decision requires one wake delay");
        }
        std::uint64_t wake_after_seconds = 0;
        if (!unsigned_integer(decision.at("wake_after_seconds"),
                              wake_after_seconds)
            || wake_after_seconds < min_wake_after_seconds
            || wake_after_seconds > max_wake_after_seconds) {
            return invalid_decision(
                std::move(result),
                "reflection wake delay must be an integer from 900 through 86400");
        }
        normalized = Json{{"schema", "gaudere.cognition.decision.v1"},
                          {"decision", "propose_wake"},
                          {"reason", reason},
                          {"wake_after_seconds", wake_after_seconds}};
    } else {
        return invalid_decision(
            std::move(result), "reflection decision is not supported");
    }

    result.content_type = bounded_reflection_decision_content_type;
    result.output = normalized.dump();
    return result;
}

} // namespace

gaudere::work::Task make_bounded_reflection_task(std::string id,
                                                  std::string objective)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = reflection_prompt(objective);
    task.limits.max_input_bytes = 16 * 1024;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = std::chrono::seconds{60};
    // Attempt two is reconciliation only. ProviderTaskHandler observes the
    // existing provider Action and never invokes the provider a second time.
    task.limits.max_attempts = 2;
    if (task.input.size() > task.limits.max_input_bytes) {
        throw std::invalid_argument(
            "bounded reflection prompt exceeds its hard input limit");
    }
    return task;
}

BoundedReflectionHandler::BoundedReflectionHandler(
    TaskHandler& provider_handler) noexcept
    : provider_handler_(provider_handler)
{
}

HandlerResult BoundedReflectionHandler::execute(const TaskContext& context)
{
    auto result = provider_handler_.execute(context);
    if (result.outcome != HandlerOutcome::succeeded) {
        return result;
    }
    return normalize_decision(std::move(result));
}

} // namespace gaudere_agent
