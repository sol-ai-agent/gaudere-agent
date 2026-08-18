#include "LocalEchoHandler.hpp"

namespace gaudere_agent {

HandlerResult LocalEchoHandler::execute(const TaskContext& context)
{
    return HandlerResult{
        HandlerOutcome::succeeded,
        context.task.input_content_type.empty() ? "text/plain" : context.task.input_content_type,
        context.task.input,
        {},
        {}};
}

} // namespace gaudere_agent
