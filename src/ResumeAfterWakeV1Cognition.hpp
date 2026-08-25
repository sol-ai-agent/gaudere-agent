#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_COGNITION_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_COGNITION_HPP

#include "ResumeAfterWakeCognition.hpp"
#include "TaskExecutor.hpp"

#include <cstdint>
#include <optional>

namespace gaudere_agent {

/** Read-only validation surface shared by the real one-shot preflight. */
[[nodiscard]] bool valid_resume_after_wake_v1_task(
    const gaudere::work::Task& task) noexcept;

/** Return the canonical frozen context capture time only for a valid v1 Task. */
[[nodiscard]] std::optional<std::int64_t> resume_after_wake_v1_captured_at_ms(
    const gaudere::work::Task& task) noexcept;

/**
 * Guarded provider-facing wrapper for one already-durable ResumeAfterWakeV1 Task.
 *
 * The v1 Task kind and canonical v2 input are validated before the borrowed
 * provider handler is invoked. Only then is the existing stop/continue
 * normalizer used. This class owns no Provider, Action store, budget, secret,
 * network client, WakeIntent store or work Runtime.
 */
class ResumeAfterWakeV1CognitionHandler final : public TaskHandler {
public:
    explicit ResumeAfterWakeV1CognitionHandler(
        TaskHandler& provider_handler) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    ResumeAfterWakeCognitionHandler normalized_provider_;
};

} // namespace gaudere_agent

#endif
