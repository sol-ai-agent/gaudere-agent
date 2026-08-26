#include "ResumeAfterWakeCognition.hpp"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_reason_bytes = 1024;
constexpr std::size_t max_objective_bytes = 4096;

bool allowed_text_controls(const std::string& value) noexcept
{
    for (const unsigned char character : value) {
        if (character < 0x20 && character != '\t'
            && character != '\n' && character != '\r') {
            return false;
        }
    }
    return true;
}

HandlerResult invalid_decision(HandlerResult result, std::string message)
{
    return HandlerResult{HandlerOutcome::failed, {}, {},
                         "cognition_invalid_resume_decision",
                         std::move(message),
                         std::move(result.metadata_content_type),
                         std::move(result.metadata)};
}

bool only_known_keys(const Json& decision)
{
    for (auto entry = decision.begin(); entry != decision.end(); ++entry) {
        if (entry.key() != "schema" && entry.key() != "decision"
            && entry.key() != "reason" && entry.key() != "objective") {
            return false;
        }
    }
    return true;
}

HandlerResult normalize_resume_decision(HandlerResult result)
{
    Json decision;
    bool duplicate_key = false;
    std::vector<std::set<std::string>> object_keys;
    const auto reject_duplicate_keys =
        [&](int, const Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) {
                object_keys.emplace_back();
            } else if (event == Json::parse_event_t::key
                       && !object_keys.empty()) {
                const auto inserted = object_keys.back().insert(
                    parsed.get<std::string>());
                duplicate_key = duplicate_key || !inserted.second;
            } else if (event == Json::parse_event_t::object_end
                       && !object_keys.empty()) {
                object_keys.pop_back();
            }
            return true;
        };

    try {
        decision = Json::parse(result.output, reject_duplicate_keys);
    } catch (...) {
        return invalid_decision(
            std::move(result), "resume output is not valid JSON");
    }

    if (duplicate_key) {
        return invalid_decision(
            std::move(result), "resume output contains a duplicate JSON key");
    }

    if (!decision.is_object() || !only_known_keys(decision)
        || !decision.contains("schema") || !decision.at("schema").is_string()
        || decision.at("schema").get<std::string>()
            != resume_after_wake_decision_schema
        || !decision.contains("decision")
        || !decision.at("decision").is_string()
        || !decision.contains("reason") || !decision.at("reason").is_string()) {
        return invalid_decision(
            std::move(result),
            "resume output does not match the decision schema");
    }

    const auto action = decision.at("decision").get<std::string>();
    const auto reason = decision.at("reason").get<std::string>();
    if (reason.empty() || reason.size() > max_reason_bytes
        || !allowed_text_controls(reason)) {
        return invalid_decision(
            std::move(result), "resume reason must be 1..1024 safe UTF-8 bytes");
    }

    Json normalized;
    if (action == "stop") {
        const bool historical_shape =
            decision.size() == 3 && !decision.contains("objective");
        const bool structured_shape =
            decision.size() == 4 && decision.contains("objective")
            && decision.at("objective").is_null();
        if (!historical_shape && !structured_shape) {
            return invalid_decision(
                std::move(result),
                "stop resume decision objective must be absent or null");
        }
        normalized = Json{{"schema", resume_after_wake_decision_schema},
                          {"decision", "stop"},
                          {"reason", reason}};
    } else if (action == "continue") {
        if (decision.size() != 4 || !decision.contains("objective")
            || !decision.at("objective").is_string()) {
            return invalid_decision(
                std::move(result),
                "continue resume decision requires one objective");
        }
        const auto objective = decision.at("objective").get<std::string>();
        if (objective.empty() || objective.size() > max_objective_bytes
            || !allowed_text_controls(objective)) {
            return invalid_decision(
                std::move(result), "resume objective must be 1..4096 safe UTF-8 bytes");
        }
        normalized = Json{{"schema", resume_after_wake_decision_schema},
                          {"decision", "continue"},
                          {"reason", reason},
                          {"objective", objective}};
    } else {
        return invalid_decision(
            std::move(result), "resume decision is not supported");
    }

    result.content_type = resume_after_wake_decision_content_type;
    result.output = normalized.dump();
    return result;
}

} // namespace

ResumeAfterWakeCognitionHandler::ResumeAfterWakeCognitionHandler(
    TaskHandler& provider_handler) noexcept
    : provider_handler_(provider_handler)
{
}

HandlerResult ResumeAfterWakeCognitionHandler::execute(
    const TaskContext& context)
{
    auto result = provider_handler_.execute(context);
    if (result.outcome != HandlerOutcome::succeeded) {
        return result;
    }
    return normalize_resume_decision(std::move(result));
}

} // namespace gaudere_agent
