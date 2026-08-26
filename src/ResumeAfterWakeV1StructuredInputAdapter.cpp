#include "ResumeAfterWakeV1StructuredInputAdapter.hpp"

#include "ResumeAfterWakeV1.hpp"

namespace gaudere_agent {

ResumeAfterWakeV1StructuredInputAdapter::ResumeAfterWakeV1StructuredInputAdapter(
    TaskHandler& downstream) noexcept
    : downstream_(downstream)
{
}

HandlerResult ResumeAfterWakeV1StructuredInputAdapter::execute(
    const TaskContext& context)
{
    if (context.task.kind != resume_after_wake_v1_task_kind
        || context.task.input_content_type != resume_after_wake_v1_content_type) {
        return HandlerResult{
            HandlerOutcome::failed, {}, {},
            "cognition_invalid_resume_provider_input",
            "v1 structured input adapter received a non-canonical Task type"};
    }

    auto adapted = context.task;
    adapted.input_content_type = "text/plain; charset=utf-8";
    return downstream_.execute(
        TaskContext{adapted, context.cancellation_requested});
}

} // namespace gaudere_agent
