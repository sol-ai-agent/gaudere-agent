#ifndef GAUDERE_AGENT_GOOSE_TOOLS_MCP_HPP
#define GAUDERE_AGENT_GOOSE_TOOLS_MCP_HPP

#include "GooseGovernanceStore.hpp"
#include "LiveControl.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

class GooseToolsMcp {
public:
    using ControlCall = std::function<LiveControlReply(const LiveControlCommand&)>;

    GooseToolsMcp(GooseGovernanceStore& governance, ControlCall control_call);

    /** Process one MCP JSON-RPC message. Notifications intentionally return nullopt. */
    [[nodiscard]] std::optional<nlohmann::json> process(
        const nlohmann::json& request);

private:
    [[nodiscard]] nlohmann::json tools_list() const;
    [[nodiscard]] nlohmann::json call_tool(
        const std::string& name,
        const nlohmann::json& arguments);
    [[nodiscard]] bool enabled_runtime_tool(const std::string& name) const;

    GooseGovernanceStore& governance_;
    ControlCall control_call_;
    std::size_t calls_this_cycle_ = 0;
};

} // namespace gaudere_agent

#endif
