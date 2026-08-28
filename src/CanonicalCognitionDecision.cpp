#include "CanonicalCognitionDecision.hpp"

#include "ResumeAfterWake.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::size_t max_objective_bytes = 4096;

bool safe_text(const std::string& value) noexcept
{
    for (const unsigned char character : value) {
        if (character < 0x20u && character != '\t'
            && character != '\n' && character != '\r') return false;
    }
    return true;
}

Json parse_without_duplicate_keys(const std::string& input)
{
    bool duplicate = false;
    std::vector<std::set<std::string>> stack;
    const auto callback = [&](int, const Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            stack.emplace_back();
        } else if (event == Json::parse_event_t::key && !stack.empty()) {
            duplicate = duplicate
                || !stack.back().insert(parsed.get<std::string>()).second;
        } else if (event == Json::parse_event_t::object_end && !stack.empty()) {
            stack.pop_back();
        }
        return true;
    };
    auto parsed = Json::parse(input, callback);
    if (duplicate) throw std::invalid_argument("duplicate JSON key");
    return parsed;
}

bool known_keys(const Json& object)
{
    if (!object.is_object()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.key() != "schema" && it.key() != "decision"
            && it.key() != "reason" && it.key() != "objective") return false;
    }
    return true;
}

} // namespace

CanonicalCognitionDecision inspect_canonical_cognition_decision(
    const std::string& output) noexcept
{
    try {
        const auto parsed = parse_without_duplicate_keys(output);
        if (!known_keys(parsed)
            || !parsed.contains("schema") || !parsed.at("schema").is_string()
            || parsed.at("schema").get<std::string>()
                != resume_after_wake_decision_schema
            || !parsed.contains("decision") || !parsed.at("decision").is_string()
            || !parsed.contains("reason") || !parsed.at("reason").is_string()) {
            return {false, {}, {}, {}, {},
                    "decision does not match canonical cognition schema"};
        }

        const auto decision = parsed.at("decision").get<std::string>();
        const auto reason = parsed.at("reason").get<std::string>();
        if (reason.empty() || reason.size() > max_reason_bytes || !safe_text(reason)) {
            return {false, {}, {}, {}, {},
                    "decision reason is outside canonical bounds"};
        }

        Json canonical;
        std::optional<std::string> objective;
        if (decision == "stop") {
            if (parsed.size() != 3 || parsed.contains("objective")) {
                return {false, {}, {}, {}, {},
                        "canonical stop decision must have exactly three keys"};
            }
            canonical = Json{{"schema", resume_after_wake_decision_schema},
                             {"decision", "stop"}, {"reason", reason}};
        } else if (decision == "continue") {
            if (parsed.size() != 4 || !parsed.contains("objective")
                || !parsed.at("objective").is_string()) {
                return {false, {}, {}, {}, {},
                        "canonical continue decision requires objective"};
            }
            objective = parsed.at("objective").get<std::string>();
            if (objective->empty() || objective->size() > max_objective_bytes
                || !safe_text(*objective)) {
                return {false, {}, {}, {}, {},
                        "decision objective is outside canonical bounds"};
            }
            canonical = Json{{"schema", resume_after_wake_decision_schema},
                             {"decision", "continue"}, {"reason", reason},
                             {"objective", *objective}};
        } else {
            return {false, {}, {}, {}, {}, "decision is unsupported"};
        }

        const auto canonical_output = canonical.dump();
        if (canonical_output != output) {
            return {false, {}, {}, {}, {},
                    "decision bytes are not canonical"};
        }
        return {true, decision, reason, objective, canonical_output, {}};
    } catch (...) {
        return {false, {}, {}, {}, {}, "decision is not strict canonical JSON"};
    }
}

} // namespace gaudere_agent
