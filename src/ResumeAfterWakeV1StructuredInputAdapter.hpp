#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_STRUCTURED_INPUT_ADAPTER_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_STRUCTURED_INPUT_ADAPTER_HPP

#include "TaskExecutor.hpp"

namespace gaudere_agent {

/** Transient MIME-only adapter for API-level Structured Outputs.
 *
 * Unlike the historical ResumeAfterWakeV1TextInputAdapter this does not prepend
 * a textual output contract. The immutable OpenAI JSON-schema contract is the
 * sole provider-level shape authority, avoiding two potentially divergent forms.
 * Durable Task identity/input/limits are never mutated.
 */
class ResumeAfterWakeV1StructuredInputAdapter final : public TaskHandler {
public:
    explicit ResumeAfterWakeV1StructuredInputAdapter(
        TaskHandler& downstream) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    TaskHandler& downstream_;
};

} // namespace gaudere_agent

#endif
