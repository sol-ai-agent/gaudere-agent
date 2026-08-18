#ifndef GAUDERE_AGENT_LOCAL_ECHO_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_ECHO_HANDLER_HPP

#include "TaskExecutor.hpp"

namespace gaudere_agent {

class LocalEchoHandler final : public TaskHandler {
public:
    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;
};

} // namespace gaudere_agent

#endif
