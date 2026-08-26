#ifndef GAUDERE_AGENT_CURRENT_COGNITION_HANDLER_HPP
#define GAUDERE_AGENT_CURRENT_COGNITION_HANDLER_HPP

#include "ResumeAfterWakeCognition.hpp"
#include "TaskExecutor.hpp"

namespace gaudere_agent {

/** Provider-boundary guard for repeatable current cognition Tasks.
 *
 * The Task itself remains proposal-only. This wrapper grants no provider
 * authority: it only validates the canonical current-cognition Task before
 * delegating to an already-authorized provider handler through the existing
 * stop/continue normalizer.
 */
class CurrentCognitionHandler final : public TaskHandler {
public:
    explicit CurrentCognitionHandler(TaskHandler& provider_handler) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    ResumeAfterWakeCognitionHandler normalized_provider_;
};

} // namespace gaudere_agent

#endif
