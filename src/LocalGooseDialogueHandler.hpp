#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_HANDLER_HPP

#include "LocalGooseRunner.hpp"
#include "TaskExecutor.hpp"

#include <string>

namespace gaudere_agent {

/**
 * Tool-free local handler for one explicit human<->Gaudere dialogue Task.
 *
 * This handler deliberately grants no MCP extension, control socket,
 * governance path, provider fallback, network authority, or scheduler authority.
 */
class LocalGooseDialogueHandler final : public TaskHandler {
public:
    LocalGooseDialogueHandler(
        LocalGooseRunner& runner,
        std::string model_id,
        std::string model_sha256,
        std::string goose_path_root = "/var/lib/gaudere/goose");

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    LocalGooseRunner& runner_;
    std::string model_id_;
    std::string model_sha256_;
    std::string goose_path_root_;
};

} // namespace gaudere_agent

#endif
