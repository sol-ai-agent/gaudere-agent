#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_TEXT_INPUT_ADAPTER_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_TEXT_INPUT_ADAPTER_HPP

#include "TaskExecutor.hpp"

namespace gaudere_agent {

/**
 * Narrow provider-boundary adapter for canonical ResumeAfterWakeV1 Tasks.
 *
 * OpenAIResponsesProvider deliberately accepts text/plain only. The durable v1
 * Task remains typed as the Gaudere canonical JSON media type. This adapter is
 * allowed to change only the transient Task copy's input content type before
 * delegating to the already-proven provider handler. Identity, idempotency key,
 * input bytes, resource limits and durable Task state are unchanged.
 */
class ResumeAfterWakeV1TextInputAdapter final : public TaskHandler {
public:
    explicit ResumeAfterWakeV1TextInputAdapter(TaskHandler& downstream) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    TaskHandler& downstream_;
};

} // namespace gaudere_agent

#endif
