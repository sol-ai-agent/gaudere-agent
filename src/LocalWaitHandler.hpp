#ifndef GAUDERE_AGENT_LOCAL_WAIT_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_WAIT_HANDLER_HPP

#include "TaskExecutor.hpp"

#include <chrono>
#include <optional>
#include <string_view>

namespace gaudere_agent {

[[nodiscard]] std::optional<std::chrono::milliseconds>
parse_local_wait_duration(std::string_view input) noexcept;

class LocalWaitHandler final : public TaskHandler {
public:
    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;
};

} // namespace gaudere_agent

#endif
