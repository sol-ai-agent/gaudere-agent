#ifndef GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HANDLER_HPP

#include "LocalGooseRunner.hpp"
#include "TaskExecutor.hpp"

#include <string>

namespace gaudere_agent {

class LocalGooseCognitionHandler final : public TaskHandler {
public:
    LocalGooseCognitionHandler(LocalGooseRunner& runner,
                               std::string model_id,
                               std::string model_sha256,
                               bool tools_enabled = false,
                               std::string control_socket = {},
                               std::string governance_path = {},
                               std::string goose_path_root = "/var/lib/gaudere/goose");

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    LocalGooseRunner& runner_;
    std::string model_id_;
    std::string model_sha256_;
    bool tools_enabled_ = false;
    std::string control_socket_;
    std::string governance_path_;
    std::string goose_path_root_;
};

} // namespace gaudere_agent

#endif
