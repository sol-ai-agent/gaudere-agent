#include "GooseGovernanceStore.hpp"
#include "GooseToolsMcp.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace gaudere_agent;

Json request(const int id,
             const std::string& method,
             Json params = Json::object())
{
    Json value = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.empty()) value["params"] = std::move(params);
    return value;
}

Json call(const int id, const std::string& name, Json arguments = Json::object())
{
    return request(id, "tools/call",
                   Json{{"name", name}, {"arguments", std::move(arguments)}});
}

Json result_of(GooseToolsMcp& server, const Json& value)
{
    const auto response = server.process(value);
    assert(response);
    assert(response->contains("result"));
    return response->at("result");
}

bool tool_list_contains(const Json& result, const std::string& name)
{
    assert(result.contains("tools") && result.at("tools").is_array());
    for (const auto& tool : result.at("tools")) {
        if (tool.value("name", "") == name) return true;
    }
    return false;
}

std::string temp_path(const char* suffix)
{
    return "/tmp/gaudere-goose-" + std::to_string(static_cast<long long>(::getpid()))
        + suffix;
}

} // namespace

int main()
{
    const auto path = temp_path("-governance.db");
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    std::vector<LiveControlCommand> controls;
    GooseGovernanceStore store(path);
    GooseToolsMcp server(
        store,
        [&controls](const LiveControlCommand& command) {
            controls.push_back(command);
            if (command.operation == LiveControlOperation::inspect_budget) {
                return LiveControlReply{true, 0, "budget-ok\n"};
            }
            if (command.operation == LiveControlOperation::submit_reflection) {
                return LiveControlReply{true, 0, "risk-review-task-submitted\n"};
            }
            return LiveControlReply{true, 0, "ok\n"};
        });

    const auto initialized = server.process(request(
        1, "initialize",
        Json{{"protocolVersion", "2025-03-26"},
             {"capabilities", Json::object()},
             {"clientInfo", Json{{"name", "test"}, {"version", "1"}}}}));
    assert(initialized);
    assert(initialized->at("result").at("protocolVersion") == "2025-03-26");
    assert(!server.process(Json{{"jsonrpc", "2.0"},
                                {"method", "notifications/initialized"}}));

    auto tools = result_of(server, request(2, "tools/list"));
    assert(tool_list_contains(tools, "gaudere_inspect_operational_policy"));
    assert(tool_list_contains(tools, "gaudere_propose_risk_rule_change"));
    assert(tool_list_contains(tools, "gaudere_local_echo"));
    assert(tool_list_contains(tools, "gaudere_request_openai_risk_review"));

    const auto risk = result_of(server, call(3, "gaudere_inspect_risk_envelope"));
    assert(!risk.at("isError").get<bool>());
    assert(risk.at("structuredContent").at("local_risk_promotion_allowed") == false);
    assert(risk.at("structuredContent").at("arbitrary_shell_allowed") == false);

    const auto changed = result_of(
        server,
        call(4, "gaudere_set_operational_policy",
             Json{{"enabled_tools", Json::array({"gaudere_inspect_budget",
                                                  "gaudere_request_openai_risk_review"})},
                  {"max_calls_per_cycle", 12}}));
    assert(!changed.at("isError").get<bool>());
    assert(changed.at("structuredContent").at("revision") == 1);
    assert(changed.at("structuredContent").at("max_calls_per_cycle") == 12);

    tools = result_of(server, request(5, "tools/list"));
    assert(tool_list_contains(tools, "gaudere_inspect_budget"));
    assert(tool_list_contains(tools, "gaudere_request_openai_risk_review"));
    assert(!tool_list_contains(tools, "gaudere_local_echo"));

    const auto disabled = result_of(
        server,
        call(6, "gaudere_local_echo", Json{{"id", "x"}, {"text", "hello"}}));
    assert(disabled.at("isError").get<bool>());
    assert(controls.empty());

    const auto budget = result_of(server, call(7, "gaudere_inspect_budget"));
    assert(!budget.at("isError").get<bool>());
    assert(budget.at("content").at(0).at("text") == "budget-ok\n");
    assert(controls.size() == 1);
    assert(controls.back().operation == LiveControlOperation::inspect_budget);

    const auto proposal = result_of(
        server,
        call(8, "gaudere_propose_risk_rule_change",
             Json{{"proposal", "Permit a hypothetical additional bounded local tool"},
                  {"reason", "Gaudere may decide the capability is useful"}}));
    assert(!proposal.at("isError").get<bool>());
    const auto proposal_id = proposal.at("structuredContent").at("id").get<std::string>();
    assert(proposal.at("structuredContent").at("status") == "pending_openai");

    const auto review = result_of(
        server,
        call(9, "gaudere_request_openai_risk_review",
             Json{{"proposal_id", proposal_id}}));
    assert(!review.at("isError").get<bool>());
    assert(review.at("structuredContent").at("status") == "review_submitted");
    assert(controls.size() == 2);
    assert(controls.back().operation == LiveControlOperation::submit_reflection);
    assert(controls.back().text.find("not authorized merely because it exists")
           != std::string::npos);

    const auto stored_proposal = store.find_risk_proposal(proposal_id);
    assert(stored_proposal);
    assert(stored_proposal->status == "review_submitted");
    assert(stored_proposal->review_task_id);

    // There is intentionally no local approval/promotion method. A submitted
    // review remains only submitted evidence until a separate OpenAI-validated
    // governance path is implemented and admitted.
    assert(stored_proposal->status != "approved");

    const auto bad_policy = result_of(
        server,
        call(10, "gaudere_set_operational_policy",
             Json{{"enabled_tools", Json::array({"arbitrary_shell"})},
                  {"max_calls_per_cycle", 2}}));
    assert(bad_policy.at("isError").get<bool>());

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    std::cout << "Goose MCP governance tests passed\n";
    return 0;
}
