#ifndef GAUDERE_AGENT_CURRENT_COGNITION_HANDLER_HPP
#define GAUDERE_AGENT_CURRENT_COGNITION_HANDLER_HPP

#include "ResumeAfterWakeCognition.hpp"
#include "TaskExecutor.hpp"

namespace gaudere_agent {

/**
 * Guard one repeatable cognition.current.v0 Task before it crosses the existing
 * provider boundary. The borrowed provider path remains the sole owner of
 * Action/budget effects; this wrapper only validates the durable Task shape and
 * reuses the existing resume-decision normalizer.
 */
class CurrentCognitionHandler final : public TaskHandler {
public:
    explicit CurrentCognitionHandler(TaskHandler& provider_handler) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    ResumeAfterWakeCognitionHandler cognition_handler_;
};

} // namespace gaudere_agent

#endif
