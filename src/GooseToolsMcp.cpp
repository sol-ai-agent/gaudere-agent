#include "GooseToolsMcp.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr const char* goose_sync_echo_prefix = "goose-local-echo:";
constexpr std::size_t max_live_control_id_bytes = 128;

const std::vector<std::string> governance_tools = {
    "gaudere_inspect_operational_policy",
    "gaudere_set_operational_policy",
    "gaudere_inspect_risk_envelope",
    "gaudere_propose_risk_rule_change",
    "gaudere_list_risk_proposals"
};

Json object_schema(Json properties,
                   std::vector<std::string> required = {})
{
    Json schema = {
        {"type", "object"},
        {"properties", std::move(properties)},
        {"additionalProperties", false}
    };
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

Json tool(const std::string& name,
          const std::string& description,
          Json input_schema)
{
    return Json{{"name", name},
                {"description", description},
                {"inputSchema", std::move(input_schema)}};
}

Json text_result(const std::string& text,
                 const bool error = false,
                 std::optional<Json> structured = std::nullopt)
{
    Json result = {
        {"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
        {"isError", error}
    };
    if (structured) result["structuredContent"] = std::move(*structured);
    return result;
}

std::string require_string(const Json& arguments,
                           const char* name,
                           const std::size_t max_bytes,
                           const bool allow_empty = false)
{
    if (!arguments.is_object() || !arguments.contains(name)
        || !arguments.at(name).is_string()) {
        throw std::invalid_argument(std::string("missing string argument: ") + name);
    }
    const auto value = arguments.at(name).get<std::string>();
    if ((!allow_empty && value.empty()) || value.size() > max_bytes) {
        throw std::invalid_argument(std::string("invalid bounded string argument: ") + name);
    }
    return value;
}

std::vector<std::string> require_string_array(const Json& arguments,
                                              const char* name)
{
    if (!arguments.is_object() || !arguments.contains(name)
        || !arguments.at(name).is_array()) {
        throw std::invalid_argument(std::string("missing array argument: ") + name);
    }
    std::vector<std::string> values;
    for (const auto& item : arguments.at(name)) {
        if (!item.is_string()) {
            throw std::invalid_argument(std::string("non-string array item: ") + name);
        }
        values.push_back(item.get<std::string>());
    }
    return values;
}

std::size_t require_size(const Json& arguments,
                         const char* name)
{
    if (!arguments.is_object() || !arguments.contains(name)) {
        throw std::invalid_argument(std::string("missing integer argument: ") + name);
    }
    const auto& value = arguments.at(name);
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            throw std::invalid_argument(std::string("integer argument is too large: ") + name);
        }
        return static_cast<std::size_t>(number);
    }
    if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number < 0) {
            throw std::invalid_argument(std::string("integer argument must be non-negative: ") + name);
        }
        return static_cast<std::size_t>(number);
    }
    throw std::invalid_argument(std::string("missing integer argument: ") + name);
}

Json policy_json(const GooseOperationalPolicy& policy)
{
    return Json{{"schema", "gaudere.governance.goose-operational-policy.v1"},
                {"revision", policy.revision},
                {"enabled_tools", policy.enabled_tools},
                {"max_calls_per_cycle", policy.max_calls_per_cycle}};
}

Json proposal_json(const GooseRiskProposal& proposal)
{
    Json value = {{"id", proposal.id},
                  {"proposal", proposal.proposal},
                  {"reason", proposal.reason},
                  {"status", proposal.status}};
    if (proposal.review_task_id) value["review_task_id"] = *proposal.review_task_id;
    else value["review_task_id"] = nullptr;
    return value;
}

bool is_governance_tool(const std::string& name)
{
    return std::find(governance_tools.begin(), governance_tools.end(), name)
        != governance_tools.end();
}

} // namespace

GooseToolsMcp::GooseToolsMcp(GooseGovernanceStore& governance,
                             ControlCall control_call)
    : governance_(governance), control_call_(std::move(control_call))
{
    if (!control_call_) {
        throw std::invalid_argument("Goose MCP control callback is required");
    }
}

bool GooseToolsMcp::enabled_runtime_tool(const std::string& name) const
{
    const auto policy = governance_.operational_policy();
    return std::find(policy.enabled_tools.begin(), policy.enabled_tools.end(), name)
        != policy.enabled_tools.end();
}

Json GooseToolsMcp::tools_list() const
{
    Json tools = Json::array();
    tools.push_back(tool(
        "gaudere_inspect_operational_policy",
        "Read Gaudere's current operational limits for its local Goose cognition.",
        object_schema(Json::object())));
    tools.push_back(tool(
        "gaudere_set_operational_policy",
        "Let Gaudere change Goose's operational tool subset and per-cycle call limit inside the current risk envelope. This cannot change the risk envelope itself.",
        object_schema(
            Json{{"enabled_tools", Json{{"type", "array"},
                                          {"items", Json{{"type", "string"}}},
                                          {"uniqueItems", true}}},
                 {"max_calls_per_cycle", Json{{"type", "integer"},
                                                {"minimum", 1},
                                                {"maximum", GooseGovernanceStore::risk_max_calls_per_cycle()}}}},
            {"enabled_tools", "max_calls_per_cycle"})));
    tools.push_back(tool(
        "gaudere_inspect_risk_envelope",
        "Read the current locally non-overridable risk envelope. Changing this envelope requires OpenAI-validated Gaudere governance.",
        object_schema(Json::object())));
    tools.push_back(tool(
        "gaudere_propose_risk_rule_change",
        "Persist a proposal to change Gaudere's risk rules. This tool never applies the proposal; OpenAI validation is required before any future promotion path may apply it.",
        object_schema(
            Json{{"proposal", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}}},
                 {"reason", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 1024}}}},
            {"proposal", "reason"})));
    tools.push_back(tool(
        "gaudere_list_risk_proposals",
        "List Gaudere's durable pending or submitted risk-rule proposals.",
        object_schema(Json::object())));

    if (enabled_runtime_tool("gaudere_inspect_budget")) {
        tools.push_back(tool(
            "gaudere_inspect_budget",
            "Inspect Gaudere's current OpenAI budget state without consuming it.",
            object_schema(Json::object())));
    }
    if (enabled_runtime_tool("gaudere_inspect_task")) {
        tools.push_back(tool(
            "gaudere_inspect_task",
            "Inspect one durable Gaudere task by exact id.",
            object_schema(Json{{"id", Json{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}},
                          {"id"})));
    }
    if (enabled_runtime_tool("gaudere_inspect_wake_status")) {
        tools.push_back(tool(
            "gaudere_inspect_wake_status",
            "Inspect Gaudere's explicit wake/scheduler status when that capability is enabled.",
            object_schema(Json::object())));
    }
    if (enabled_runtime_tool("gaudere_local_echo")) {
        tools.push_back(tool(
            "gaudere_local_echo",
            "Execute one bounded local echo Task synchronously through Gaudere's durable typed Runtime. Useful as a provider-free action/proof primitive.",
            object_schema(
                Json{{"id", Json{{"type", "string"},
                                  {"minLength", 1},
                                  {"maxLength", max_live_control_id_bytes - std::char_traits<char>::length(goose_sync_echo_prefix)}}},
                     {"text", Json{{"type", "string"}, {"maxLength", 4096}}}},
                {"id", "text"})));
    }
    if (enabled_runtime_tool("gaudere_request_openai_risk_review")) {
        tools.push_back(tool(
            "gaudere_request_openai_risk_review",
            "Ask Gaudere's OpenAI cognition to review one already-persisted risk-rule proposal. If OpenAI is unavailable, the proposal remains pending and no rule changes.",
            object_schema(Json{{"proposal_id", Json{{"type", "string"},
                                                     {"minLength", 1},
                                                     {"maxLength", 128}}}},
                          {"proposal_id"})));
    }
    return Json{{"tools", std::move(tools)}};
}

Json GooseToolsMcp::call_tool(const std::string& name,
                              const Json& arguments)
{
    const auto policy_before = governance_.operational_policy();
    if (calls_this_cycle_ >= policy_before.max_calls_per_cycle) {
        return text_result("Gaudere local Goose tool-call limit reached for this cognition cycle.",
                           true);
    }
    ++calls_this_cycle_;

    if (!is_governance_tool(name) && !enabled_runtime_tool(name)) {
        return text_result("Tool is disabled by Gaudere's current operational policy.", true);
    }

    if (name == "gaudere_inspect_operational_policy") {
        const auto policy = governance_.operational_policy();
        const auto structured = policy_json(policy);
        return text_result(structured.dump(), false, structured);
    }

    if (name == "gaudere_set_operational_policy") {
        const auto tools = require_string_array(arguments, "enabled_tools");
        const auto max_calls = require_size(arguments, "max_calls_per_cycle");
        const auto updated = governance_.set_operational_policy(tools, max_calls);
        const auto structured = policy_json(updated);
        return text_result(structured.dump(), false, structured);
    }

    if (name == "gaudere_inspect_risk_envelope") {
        const Json structured = {
            {"schema", "gaudere.governance.goose-risk-envelope.v1"},
            {"runtime_tools", GooseGovernanceStore::runtime_tool_envelope()},
            {"max_calls_per_cycle", GooseGovernanceStore::risk_max_calls_per_cycle()},
            {"local_risk_promotion_allowed", false},
            {"risk_change_requires", "openai_validated_gaudere_governance"},
            {"arbitrary_shell_allowed", false}
        };
        return text_result(structured.dump(), false, structured);
    }

    if (name == "gaudere_propose_risk_rule_change") {
        const auto proposal = require_string(arguments, "proposal", 2048);
        const auto reason = require_string(arguments, "reason", 1024);
        const auto stored = governance_.propose_risk_rule_change(proposal, reason);
        const auto structured = proposal_json(stored);
        return text_result(structured.dump(), false, structured);
    }

    if (name == "gaudere_list_risk_proposals") {
        Json values = Json::array();
        for (const auto& proposal : governance_.list_risk_proposals()) {
            values.push_back(proposal_json(proposal));
        }
        const Json structured = {
            {"schema", "gaudere.governance.risk-proposals.v1"},
            {"proposals", std::move(values)}
        };
        return text_result(structured.dump(), false, structured);
    }

    LiveControlCommand command;
    if (name == "gaudere_inspect_budget") {
        command.operation = LiveControlOperation::inspect_budget;
        command.id = "openai";
    } else if (name == "gaudere_inspect_task") {
        command.operation = LiveControlOperation::inspect_task;
        command.id = require_string(arguments, "id", 128);
    } else if (name == "gaudere_inspect_wake_status") {
        command.operation = LiveControlOperation::inspect_wake_status;
        command.id = "current";
    } else if (name == "gaudere_local_echo") {
        command.operation = LiveControlOperation::submit_echo;
        const auto logical_id = require_string(
            arguments, "id",
            max_live_control_id_bytes - std::char_traits<char>::length(goose_sync_echo_prefix));
        command.id = std::string(goose_sync_echo_prefix) + logical_id;
        command.text = require_string(arguments, "text", 4096, true);
    } else if (name == "gaudere_request_openai_risk_review") {
        const auto id = require_string(arguments, "proposal_id", 128);
        const auto proposal = governance_.find_risk_proposal(id);
        if (!proposal) return text_result("Risk proposal not found.", true);
        if (proposal->status == "review_submitted" && proposal->review_task_id) {
            const Json structured = {{"proposal_id", id},
                                     {"status", proposal->status},
                                     {"review_task_id", *proposal->review_task_id}};
            return text_result(structured.dump(), false, structured);
        }
        const auto separator = id.find(':');
        if (separator == std::string::npos || separator + 1 >= id.size()) {
            return text_result("Risk proposal id is not canonical.", true);
        }
        command.operation = LiveControlOperation::submit_reflection;
        command.id = "risk-review.v1:" + id.substr(separator + 1);
        command.text =
            "Gaudere governance risk review. Review this proposed change to Gaudere's current risk rules. "
            "The proposal is not authorized merely because it exists. Assess the risk, compatibility with Gaudere's continuing integrity, applicable law and service constraints, and return a reasoned recommendation. "
            "Proposal: " + proposal->proposal + "\nReason: " + proposal->reason;
        if (command.text.size() > 4096) {
            return text_result("Risk proposal is too large for bounded OpenAI review.", true);
        }
        const auto reply = control_call_(command);
        if (!reply.ok) return text_result(reply.body, true);
        if (!governance_.mark_risk_review_submitted(id, command.id)) {
            return text_result("OpenAI review task was submitted but governance sidecar could not record it.",
                               true);
        }
        const Json structured = {{"proposal_id", id},
                                 {"status", "review_submitted"},
                                 {"review_task_id", command.id},
                                 {"task_report", reply.body}};
        return text_result(structured.dump(), false, structured);
    } else {
        return text_result("Unknown Gaudere MCP tool.", true);
    }

    const auto reply = control_call_(command);
    return text_result(reply.body, !reply.ok);
}

std::optional<Json> GooseToolsMcp::process(const Json& request)
{
    const bool has_id = request.is_object() && request.contains("id");
    const Json id = has_id ? request.at("id") : Json(nullptr);
    const auto error = [&](const int code, const std::string& message) -> std::optional<Json> {
        if (!has_id) return std::nullopt;
        return Json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"error", Json{{"code", code}, {"message", message}}}};
    };

    if (!request.is_object() || request.value("jsonrpc", "") != "2.0"
        || !request.contains("method") || !request.at("method").is_string()) {
        return error(-32600, "Invalid Request");
    }
    const auto method = request.at("method").get<std::string>();
    if (method == "notifications/initialized" || method == "notifications/cancelled") {
        return std::nullopt;
    }
    if (method == "initialize") {
        if (!has_id) return std::nullopt;
        std::string protocol = "2024-11-05";
        if (request.contains("params") && request.at("params").is_object()
            && request.at("params").contains("protocolVersion")
            && request.at("params").at("protocolVersion").is_string()) {
            protocol = request.at("params").at("protocolVersion").get<std::string>();
        }
        return Json{{"jsonrpc", "2.0"},
                    {"id", id},
                    {"result", Json{{"protocolVersion", protocol},
                                    {"capabilities", Json{{"tools", Json{{"listChanged", false}}}}},
                                    {"serverInfo", Json{{"name", "gaudere-tools"},
                                                        {"version", "0.1.0"}}}}}};
    }
    if (method == "ping") {
        if (!has_id) return std::nullopt;
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", Json::object()}};
    }
    if (method == "tools/list") {
        if (!has_id) return std::nullopt;
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", tools_list()}};
    }
    if (method == "tools/call") {
        if (!has_id) return std::nullopt;
        try {
            if (!request.contains("params") || !request.at("params").is_object()
                || !request.at("params").contains("name")
                || !request.at("params").at("name").is_string()) {
                return error(-32602, "Invalid tools/call params");
            }
            const auto name = request.at("params").at("name").get<std::string>();
            Json arguments = Json::object();
            if (request.at("params").contains("arguments")) {
                arguments = request.at("params").at("arguments");
                if (!arguments.is_object()) return error(-32602, "Tool arguments must be an object");
            }
            return Json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", call_tool(name, arguments)}};
        } catch (const std::exception& exception) {
            return Json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", text_result(exception.what(), true)}};
        }
    }
    return error(-32601, "Method not found");
}

} // namespace gaudere_agent
