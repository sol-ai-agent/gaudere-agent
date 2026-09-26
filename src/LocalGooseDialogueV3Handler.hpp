#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V3_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V3_HANDLER_HPP

#include "LocalGooseDialogueV3.hpp"
#include "LocalGooseRunner.hpp"
#include "TaskExecutor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gaudere_agent {

using LocalGooseDialogueV3TaskLookup =
    std::function<std::optional<gaudere::work::Task>(const std::string&)>;

struct LocalGooseDialogueV3HistoryTurn {
    std::uint64_t turn_index = 0;
    std::string task_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string message;
    std::string assistant_response;
};

struct LocalGooseDialogueV3History {
    bool eligible = false;
    bool truncated = false;
    std::string detail;
    std::vector<LocalGooseDialogueV3HistoryTurn> turns;
};

[[nodiscard]] LocalGooseDialogueV3History
resolve_local_goose_dialogue_v3_history(
    const gaudere::work::Task& current,
    const LocalGooseDialogueV3TaskLookup& lookup) noexcept;

/**
 * Tool-free handler for one explicit V3 multi-actor dialogue Task.
 *
 * It resolves a bounded mixed V2/V3 predecessor lineage. Legacy V2 turns are
 * rendered as legacy-v2-human (human/dialogue) because V2 did not durably
 * encode a speaker identity. V3 turns preserve their canonical actor/message
 * provenance.
 *
 * The handler grants no MCP tools, control socket, governance path, network,
 * provider fallback, shell, secrets, scheduler authority, or autonomous-cycle
 * authority.
 */
class LocalGooseDialogueV3Handler final : public TaskHandler {
public:
    LocalGooseDialogueV3Handler(
        LocalGooseRunner& runner,
        LocalGooseDialogueV3TaskLookup lookup,
        std::string model_id,
        std::string model_sha256,
        std::string goose_path_root = "/var/lib/gaudere/goose");

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    LocalGooseRunner& runner_;
    LocalGooseDialogueV3TaskLookup lookup_;
    std::string model_id_;
    std::string model_sha256_;
    std::string goose_path_root_;
};

} // namespace gaudere_agent

#endif
