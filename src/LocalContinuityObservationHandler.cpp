#include "LocalContinuityObservationHandler.hpp"

#include "LocalContinuityObservation.hpp"

namespace gaudere_agent {

HandlerResult LocalContinuityObservationHandler::execute(const TaskContext& context)
{
    if (context.cancellation_requested && context.cancellation_requested())
        return {HandlerOutcome::cancelled, {}, {}, {}, {}};

    const auto inspection = inspect_local_continuity_observation_task(context.task);
    if (!inspection.eligible) {
        return {HandlerOutcome::failed,
                {},
                {},
                "invalid_local_continuity_observation",
                inspection.detail.empty()
                    ? "local continuity observation Task is not canonical"
                    : inspection.detail};
    }

    return {HandlerOutcome::succeeded,
            local_continuity_observation_content_type,
            context.task.input,
            {}, {}};
}

} // namespace gaudere_agent
