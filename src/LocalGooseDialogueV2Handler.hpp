#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V2_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V2_HANDLER_HPP

#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseRunner.hpp"
#include "TaskExecutor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gaudere_agent {

using LocalGooseDialogueV2TaskLookup =
    std::function<std::optional<gaudere::work::Task>(const std::string&)>;

struct LocalGooseDialogueV2HistoryTurn {
    std::uint64_t turn_index = 0;
    std::string task_id;
    std::string human_message;
    std::string assistant_response;
};

struct LocalGooseDialogueV2History {
    bool eligible = false;
    bool truncated = false;
    std::string detail;
    std::vector<LocalGooseDialogueV2HistoryTurn> turns;
};

[[nodiscard]] LocalGooseDialogueV2History
resolve_local_goose_dialogue_v2_history(
    const gaudere::work::Task& current,
    const LocalGooseDialogueV2TaskLookup& lookup) noexcept;

/**
 * Tool-free handler for one explicit V2 multi-turn dialogue Task.
 *
 * It resolves durable predecessor evidence through a bounded lookup callback,
 * builds only a bounded conversational context, and grants no MCP tools,
 * control socket, governance path, network, provider fallback, shell, secrets,
 * scheduler authority, or autonomous-cycle authority.
 */
class LocalGooseDialogueV2Handler final : public TaskHandler {
public:
    LocalGooseDialogueV2Handler(
        LocalGooseRunner& runner,
        LocalGooseDialogueV2TaskLookup lookup,
        std::string model_id,
        std::string model_sha256,
        std::string goose_path_root = "/var/lib/gaudere/goose");

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    LocalGooseRunner& runner_;
    LocalGooseDialogueV2TaskLookup lookup_;
    std::string model_id_;
    std::string model_sha256_;
    std::string goose_path_root_;
};

} // namespace gaudere_agent

#endif
