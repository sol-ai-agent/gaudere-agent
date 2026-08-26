#include "CurrentCognitionHandler.hpp"

#include "CurrentCognitionCycle.hpp"

namespace gaudere_agent {

CurrentCognitionHandler::CurrentCognitionHandler(
    TaskHandler& provider_handler) noexcept
    : cognition_handler_(provider_handler)
{
}

HandlerResult CurrentCognitionHandler::execute(const TaskContext& context)
{
    if (!valid_current_cognition_task(context.task)) {
        return HandlerResult{
            HandlerOutcome::failed,
            {},
            {},
            "cognition_invalid_current_task",
            "current cognition Task is not canonical",
            {},
            {}};
    }
    return cognition_handler_.execute(context);
}

} // namespace gaudere_agent
