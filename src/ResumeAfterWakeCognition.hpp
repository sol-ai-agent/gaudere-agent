#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_COGNITION_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_COGNITION_HPP

#include "ResumeAfterWake.hpp"
#include "TaskExecutor.hpp"

namespace gaudere_agent {

inline constexpr const char* resume_after_wake_decision_content_type =
    "application/vnd.gaudere.cognition-resume-decision+json";

/** Validate and normalize the definite output of an existing provider handler.
 *
 * This wrapper owns no provider, Action runtime, budget, WakeIntent store, or
 * work Runtime. It delegates exactly one bounded Task to the borrowed handler
 * and, only after a definite success, canonicalizes the model proposal into the
 * resume-after-wake decision schema. Invalid model output is a normal durable
 * Task failure; ambiguous provider effects remain manual_review unchanged.
 */
class ResumeAfterWakeCognitionHandler final : public TaskHandler {
public:
    explicit ResumeAfterWakeCognitionHandler(
        TaskHandler& provider_handler) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    TaskHandler& provider_handler_;
};

} // namespace gaudere_agent

#endif
