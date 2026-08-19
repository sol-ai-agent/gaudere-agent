#include "LiveControlProcessor.hpp"

#include "OpenAIOneShot.hpp"
#include "TaskReport.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

gaudere::work::Task make_live_echo_task(const LiveControlCommand& command)
{
    gaudere::work::Task task;
    task.id = command.id;
    task.idempotency_key = "local.echo:" + command.id;
    task.kind = "local.echo";
    task.input_content_type = "text/plain";
    task.input = command.text;
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = std::chrono::seconds{1};
    task.limits.max_attempts = 1;
    return task;
}

std::string task_report(const gaudere::work::Task& task)
{
    std::ostringstream output;
    print_task_report(output, task);
    return output.str();
}

LiveControlReply not_found()
{
    return LiveControlReply{false, 3, "gaudere-agent: task not found\n"};
}

} // namespace

LiveControlProcessor::LiveControlProcessor(gaudere::work::Runtime& runtime,
                                           gaudere::work::TaskStore& store,
                                           const bool openai_enabled)
    : runtime_(runtime), store_(store), openai_enabled_(openai_enabled)
{
}

LiveControlProcessResult LiveControlProcessor::process(LiveControlMailbox& mailbox)
{
    LiveControlProcessResult result;
    for (const auto& pending : mailbox.take_all()) {
        ++result.processed;
        bool work_may_be_pending = false;
        try {
            pending->complete(process_one(pending->command(), work_may_be_pending));
        } catch (const std::exception& error) {
            pending->complete(LiveControlReply{
                false, 1, std::string("gaudere-agent: live control command failed: ")
                              + error.what() + "\n"});
        } catch (...) {
            pending->complete(LiveControlReply{
                false, 1,
                "gaudere-agent: live control command failed with non-standard exception\n"});
        }
        result.work_may_be_pending = result.work_may_be_pending || work_may_be_pending;
    }
    return result;
}

LiveControlReply LiveControlProcessor::process_one(const LiveControlCommand& command,
                                                   bool& work_may_be_pending)
{
    if (command.operation == LiveControlOperation::inspect_task) {
        const auto task = store_.find(command.id);
        return task
            ? LiveControlReply{true, 0, task_report(*task)}
            : not_found();
    }

    gaudere::work::Task task;
    std::string description;
    switch (command.operation) {
    case LiveControlOperation::submit_echo:
        task = make_live_echo_task(command);
        description = "local.echo";
        break;
    case LiveControlOperation::submit_openai:
        if (!openai_enabled_) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: OpenAI provider is not enabled in this service\n"};
        }
        task = make_openai_task(command.id, command.text);
        description = "OpenAI Responses";
        break;
    case LiveControlOperation::inspect_task:
        throw std::logic_error("inspect_task unexpectedly reached submit path");
    }

    const auto id = task.id;
    const auto submit = runtime_.submit(task);
    if (submit != gaudere::work::SubmitResult::accepted
        && submit != gaudere::work::SubmitResult::duplicate) {
        return LiveControlReply{
            false, 4, "gaudere-agent: " + description + " submission rejected\n"};
    }

    const auto stored = store_.find(id);
    if (!stored) {
        throw std::runtime_error(description + " task is missing after submission");
    }

    // A duplicate can still refer to pending/recoverable work. Waking the normal
    // dispatcher is cheap and preserves the event-driven no-polling model.
    if (!gaudere::work::is_terminal(stored->status)) {
        work_may_be_pending = true;
    }
    return LiveControlReply{true, 0, task_report(*stored)};
}

} // namespace gaudere_agent
