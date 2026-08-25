#include "ResumeAfterWakeV1TextInputAdapter.hpp"

#include "ResumeAfterWakeV1.hpp"

#include <string>

namespace gaudere_agent {

ResumeAfterWakeV1TextInputAdapter::ResumeAfterWakeV1TextInputAdapter(
    TaskHandler& downstream) noexcept
    : downstream_(downstream)
{
}

HandlerResult ResumeAfterWakeV1TextInputAdapter::execute(
    const TaskContext& context)
{
    if (context.task.kind != resume_after_wake_v1_task_kind
        || context.task.input_content_type != resume_after_wake_v1_content_type) {
        return HandlerResult{
            HandlerOutcome::failed, {}, {},
            "cognition_invalid_resume_provider_input",
            "v1 provider input adapter received a non-canonical Task type"};
    }

    auto adapted = context.task;
    adapted.input_content_type = "text/plain; charset=utf-8";
    return downstream_.execute(
        TaskContext{adapted, context.cancellation_requested});
}

} // namespace gaudere_agent
