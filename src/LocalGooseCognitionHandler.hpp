#ifndef GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HANDLER_HPP

#include "LocalGooseRunner.hpp"
#include "TaskExecutor.hpp"

#include <string>

namespace gaudere_agent {

class LocalGooseCognitionHandler final : public TaskHandler {
public:
    LocalGooseCognitionHandler(LocalGooseRunner& runner,
                               std::string model_path,
                               std::string model_sha256,
                               bool tools_enabled = false,
                               std::string control_socket = {},
                               std::string governance_path = {});

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    LocalGooseRunner& runner_;
    std::string model_path_;
    std::string model_sha256_;
    bool tools_enabled_ = false;
    std::string control_socket_;
    std::string governance_path_;
};

} // namespace gaudere_agent

#endif
