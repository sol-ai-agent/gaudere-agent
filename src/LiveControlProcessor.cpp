#include "LiveControlProcessor.hpp"

#include "BoundedReflection.hpp"
#include "OpenAIBudget.hpp"
#include "OpenAITask.hpp"
#include "TaskReport.hpp"

#include <algorithm>
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

std::string admission_name(const gaudere::budget::ConsumeResult result)
{
    switch (result) {
    case gaudere::budget::ConsumeResult::accepted:
        return "available";
    case gaudere::budget::ConsumeResult::total_exhausted:
        return "total_exhausted";
    case gaudere::budget::ConsumeResult::window_exhausted:
        return "window_exhausted";
    case gaudere::budget::ConsumeResult::cooldown:
        return "cooldown";
    case gaudere::budget::ConsumeResult::clock_rollback:
        return "clock_rollback";
    case gaudere::budget::ConsumeResult::duplicate:
        return "invalid_duplicate";
    }
    return "invalid";
}

std::string budget_report(const gaudere::budget::Snapshot& snapshot,
                          const gaudere::budget::Policy& policy,
                          const bool provider_enabled)
{
    const auto remaining_total = policy.max_total
        - std::min(policy.max_total, snapshot.total_used);
    const auto remaining_window = policy.max_in_window
        - std::min(policy.max_in_window, snapshot.in_window_used);

    std::ostringstream output;
    output << "scope=\"" << openai_budget_scope() << "\"\n"
           << "provider_enabled=" << (provider_enabled ? "true" : "false") << '\n'
           << "max_total=" << policy.max_total << '\n'
           << "total_used=" << snapshot.total_used << '\n'
           << "remaining_total=" << remaining_total << '\n'
           << "max_window=" << policy.max_in_window << '\n'
           << "window_seconds="
           << std::chrono::duration_cast<std::chrono::seconds>(policy.window).count()
           << '\n'
           << "in_window_used=" << snapshot.in_window_used << '\n'
           << "remaining_window=" << remaining_window << '\n'
           << "min_interval_seconds="
           << std::chrono::duration_cast<std::chrono::seconds>(policy.min_interval).count()
           << '\n';
    if (snapshot.last_consumed_at) {
        output << "last_consumed_at_ms="
               << std::chrono::duration_cast<std::chrono::milliseconds>(
                      snapshot.last_consumed_at->time_since_epoch()).count()
               << '\n';
    } else {
        output << "last_consumed_at_ms=none\n";
    }
    output << "next_new_call=" << admission_name(snapshot.next_new_consumption) << '\n';
    return output.str();
}

LiveControlReply not_found()
{
    return LiveControlReply{false, 3, "gaudere-agent: task not found\n"};
}

} // namespace

LiveControlProcessor::LiveControlProcessor(gaudere::work::Runtime& runtime,
                                           gaudere::work::TaskStore& store,
                                           gaudere::budget::Store& budget_store,
                                           gaudere::budget::Policy budget_policy,
                                           const bool openai_enabled)
    : runtime_(runtime),
      store_(store),
      budget_store_(budget_store),
      budget_policy_(std::move(budget_policy)),
      openai_enabled_(openai_enabled)
{
    if (!gaudere::budget::valid_policy(budget_policy_)) {
        throw std::invalid_argument("live control provider budget policy is invalid");
    }
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

    if (command.operation == LiveControlOperation::inspect_budget) {
        const auto snapshot = budget_store_.snapshot(
            std::string(openai_budget_scope()),
            std::chrono::system_clock::now(), budget_policy_);
        return LiveControlReply{
            true, 0, budget_report(snapshot, budget_policy_, openai_enabled_)};
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
    case LiveControlOperation::submit_reflection:
        if (!openai_enabled_) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: OpenAI provider is not enabled in this service\n"};
        }
        task = make_bounded_reflection_task(command.id, command.text);
        description = "bounded reflection";
        break;
    case LiveControlOperation::inspect_task:
    case LiveControlOperation::inspect_budget:
        throw std::logic_error("inspect operation unexpectedly reached submit path");
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
