#include "LiveControlProcessor.hpp"

#include "BoundedReflection.hpp"
#include "LocalEchoHandler.hpp"
#include "LocalGooseDialogue.hpp"
#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "OpenAIBudget.hpp"
#include "OpenAITask.hpp"
#include "TaskExecutor.hpp"
#include "TaskReport.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr const char* goose_sync_echo_prefix = "goose-local-echo:";

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

bool goose_synchronous_echo(const LiveControlCommand& command) noexcept
{
    return command.operation == LiveControlOperation::submit_echo
        && command.id.rfind(goose_sync_echo_prefix, 0) == 0;
}

bool same_local_echo_definition(const gaudere::work::Task& stored,
                                const gaudere::work::Task& expected) noexcept
{
    return stored.id == expected.id
        && stored.idempotency_key == expected.idempotency_key
        && stored.kind == expected.kind
        && stored.input_content_type == expected.input_content_type
        && stored.input == expected.input
        && stored.limits.max_input_bytes == expected.limits.max_input_bytes
        && stored.limits.max_output_bytes == expected.limits.max_output_bytes
        && stored.limits.max_runtime == expected.limits.max_runtime
        && stored.limits.max_attempts == expected.limits.max_attempts;
}

bool same_local_goose_dialogue_definition(
    const gaudere::work::Task& stored,
    const gaudere::work::Task& expected) noexcept
{
    return stored.id == expected.id
        && stored.idempotency_key == expected.idempotency_key
        && stored.kind == expected.kind
        && stored.input_content_type == expected.input_content_type
        && stored.input == expected.input
        && stored.limits.max_input_bytes == expected.limits.max_input_bytes
        && stored.limits.max_output_bytes == expected.limits.max_output_bytes
        && stored.limits.max_runtime == expected.limits.max_runtime
        && stored.limits.max_attempts == expected.limits.max_attempts;
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

LiveControlReply wake_disabled()
{
    return LiveControlReply{
        false, 4,
        "gaudere-agent: explicit wake capability is not enabled in this service\n"};
}

LiveControlReply wake_not_found()
{
    return LiveControlReply{false, 3, "gaudere-agent: wake not found\n"};
}

std::string wake_acceptance_name(const ExplicitWakeAcceptResult result)
{
    switch (result) {
    case ExplicitWakeAcceptResult::accepted:
        return "accepted";
    case ExplicitWakeAcceptResult::duplicate:
        return "duplicate";
    case ExplicitWakeAcceptResult::source_not_found:
        return "source_not_found";
    case ExplicitWakeAcceptResult::source_ineligible:
        return "source_ineligible";
    case ExplicitWakeAcceptResult::total_exhausted:
        return "total_exhausted";
    case ExplicitWakeAcceptResult::conflict:
        return "conflict";
    case ExplicitWakeAcceptResult::invalid:
        return "invalid";
    }
    throw std::invalid_argument("unknown explicit wake acceptance result");
}

std::string wake_revoke_name(
    const gaudere::scheduling::wake::WakeIntentRevokeResult result)
{
    using Result = gaudere::scheduling::wake::WakeIntentRevokeResult;
    switch (result) {
    case Result::revoked:
        return "revoked";
    case Result::fired:
        return "fired";
    case Result::manual_review:
        return "manual_review";
    case Result::not_found:
        return "not_found";
    case Result::terminal:
        return "terminal";
    case Result::invalid:
        return "invalid";
    }
    throw std::invalid_argument("unknown wake-intent revoke result");
}

std::optional<std::string> canonical_dialogue_root(
    const gaudere::work::Task& task) noexcept
{
    if (canonical_local_goose_dialogue_v2_success(task) && task.result) {
        const auto response = inspect_local_goose_dialogue_v2_response(
            task, task.result->output);
        if (response.eligible) return response.root_task_id;
    }
    if (canonical_local_goose_dialogue_v3_success(task) && task.result) {
        const auto response = inspect_local_goose_dialogue_v3_response(
            task, task.result->output);
        if (response.eligible) return response.root_task_id;
    }
    return std::nullopt;
}

bool same_preferred_v3_request(
    const gaudere::work::Task& task,
    const LiveControlCommand& command) noexcept
{
    const auto dialogue = inspect_local_goose_dialogue_v3_task(task);
    return dialogue.eligible
        && dialogue.request_id == command.id
        && dialogue.speaker_kind == command.speaker_kind
        && dialogue.speaker_id == command.speaker_id
        && dialogue.message_kind == command.message_kind
        && dialogue.message == command.text;
}

LiveControlReply thread_store_disabled()
{
    return LiveControlReply{
        false, 4,
        "gaudere-agent: preferred dialogue thread capability is not enabled in this service\n"};
}


} // namespace

LiveControlProcessor::LiveControlProcessor(gaudere::work::Runtime& runtime,
                                           gaudere::work::TaskStore& store,
                                           gaudere::budget::Store& budget_store,
                                           gaudere::budget::Policy budget_policy,
                                           const bool openai_enabled,
                                           ExplicitWake* explicit_wake,
                                           SchedulerNext scheduler_next,
                                           LocalGooseStimulus local_goose_stimulus,
                                           std::string local_goose_dialogue_model_sha256,
                                           LocalGooseDialogueThreadStore* dialogue_thread_store)
    : runtime_(runtime),
      store_(store),
      budget_store_(budget_store),
      budget_policy_(std::move(budget_policy)),
      openai_enabled_(openai_enabled),
      explicit_wake_(explicit_wake),
      scheduler_next_(std::move(scheduler_next)),
      local_goose_stimulus_(std::move(local_goose_stimulus)),
      local_goose_dialogue_model_sha256_(
          std::move(local_goose_dialogue_model_sha256)),
      dialogue_thread_store_(dialogue_thread_store)
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
        bool wake_deadline_may_have_changed = false;
        bool local_goose_cycle_may_have_changed = false;
        const auto operation = pending->command().operation;
        const bool task_submission_may_have_committed =
            operation == LiveControlOperation::submit_echo
            || operation == LiveControlOperation::submit_openai
            || operation == LiveControlOperation::submit_reflection
            || operation == LiveControlOperation::submit_local_goose_dialogue
            || operation == LiveControlOperation::submit_local_goose_dialogue_v2_root
            || operation == LiveControlOperation::submit_local_goose_dialogue_v2_next
            || operation == LiveControlOperation::submit_local_goose_dialogue_v3_root
            || operation == LiveControlOperation::submit_local_goose_dialogue_v3_next
            || operation == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next;
        const bool wake_transition_may_have_committed =
            operation == LiveControlOperation::accept_wake
            || operation == LiveControlOperation::revoke_wake;
        const bool local_goose_transition_may_have_committed =
            operation == LiveControlOperation::stimulate_local_goose_cycle;
        try {
            pending->complete(process_one(
                pending->command(), work_may_be_pending,
                wake_deadline_may_have_changed,
                local_goose_cycle_may_have_changed));
        } catch (const std::exception& error) {
            work_may_be_pending = task_submission_may_have_committed
                && !goose_synchronous_echo(pending->command());
            wake_deadline_may_have_changed =
                wake_transition_may_have_committed;
            local_goose_cycle_may_have_changed =
                local_goose_transition_may_have_committed;
            pending->complete(LiveControlReply{
                false, 1, std::string("gaudere-agent: live control command failed: ")
                              + error.what() + "\n"});
        } catch (...) {
            work_may_be_pending = task_submission_may_have_committed
                && !goose_synchronous_echo(pending->command());
            wake_deadline_may_have_changed =
                wake_transition_may_have_committed;
            local_goose_cycle_may_have_changed =
                local_goose_transition_may_have_committed;
            pending->complete(LiveControlReply{
                false, 1,
                "gaudere-agent: live control command failed with non-standard exception\n"});
        }
        result.work_may_be_pending = result.work_may_be_pending || work_may_be_pending;
        result.wake_deadline_may_have_changed =
            result.wake_deadline_may_have_changed
            || wake_deadline_may_have_changed;
        result.local_goose_cycle_may_have_changed =
            result.local_goose_cycle_may_have_changed
            || local_goose_cycle_may_have_changed;
    }
    return result;
}

LiveControlReply LiveControlProcessor::process_one(
    const LiveControlCommand& command,
    bool& work_may_be_pending,
    bool& wake_deadline_may_have_changed,
    bool& local_goose_cycle_may_have_changed)
{
    if (command.operation
        == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
        if (!dialogue_thread_store_) return thread_store_disabled();
        const auto head = dialogue_thread_store_->find(command.id);
        return head
            ? LiveControlReply{
                true, 0, local_goose_dialogue_thread_head_report(*head)}
            : LiveControlReply{
                false, 3, "gaudere-agent: dialogue thread alias not found\n"};
    }

    if (command.operation
        == LiveControlOperation::bind_local_goose_dialogue_thread_head) {
        if (!dialogue_thread_store_) return thread_store_disabled();
        const auto task = store_.find(command.predecessor_task_id);
        if (!task) {
            return LiveControlReply{
                false, 3,
                "gaudere-agent: dialogue thread bind head Task not found\n"};
        }
        const auto root = canonical_dialogue_root(*task);
        if (!root) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: dialogue thread bind requires canonical successful V2/V3 head\n"};
        }
        LocalGooseDialogueThreadHead head{
            command.id, 0, *root, task->id};
        const auto write = dialogue_thread_store_->seed(head);
        switch (write.result) {
        case LocalGooseDialogueThreadStoreResult::accepted:
        case LocalGooseDialogueThreadStoreResult::duplicate:
            return LiveControlReply{
                true, 0,
                local_goose_dialogue_thread_head_report(*write.head)};
        case LocalGooseDialogueThreadStoreResult::conflict:
            return LiveControlReply{
                false, 4,
                std::string("gaudere-agent: dialogue thread bind conflict\n")
                    + (write.head
                        ? local_goose_dialogue_thread_head_report(*write.head)
                        : std::string{})};
        case LocalGooseDialogueThreadStoreResult::invalid:
        case LocalGooseDialogueThreadStoreResult::unavailable:
            return LiveControlReply{
                false, 4,
                "gaudere-agent: dialogue thread bind failed: "
                    + write.detail + "\n"};
        }
    }

    if (command.operation
        == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        if (!dialogue_thread_store_) return thread_store_disabled();

        auto head = dialogue_thread_store_->find(command.thread_alias);
        if (!head) {
            return LiveControlReply{
                false, 3,
                "gaudere-agent: preferred dialogue thread alias not found\n"};
        }
        const auto expected_revision = *command.expected_thread_revision;
        if (head->revision != expected_revision) {
            if (expected_revision
                    < static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())
                && head->revision == expected_revision + 1) {
                const auto current_task = store_.find(head->head_task_id);
                if (current_task
                    && same_preferred_v3_request(*current_task, command)) {
                    if (!gaudere::work::is_terminal(current_task->status)) {
                        work_may_be_pending = true;
                    }
                    return LiveControlReply{
                        true, 0,
                        task_report(*current_task)
                            + local_goose_dialogue_thread_head_report(*head)};
                }
            }
            return LiveControlReply{
                false, 4,
                "gaudere-agent: preferred dialogue thread revision conflict\n"
                    + local_goose_dialogue_thread_head_report(*head)};
        }
        if (head->revision
            == static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: preferred dialogue thread revision exhausted\n"};
        }

        const auto predecessor = store_.find(head->head_task_id);
        if (!predecessor) {
            return LiveControlReply{
                false, 3,
                "gaudere-agent: preferred dialogue predecessor Task not found\n"};
        }
        const auto root = canonical_dialogue_root(*predecessor);
        if (!root || *root != head->root_task_id) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: preferred dialogue head is not canonical success for declared root\n"};
        }

        gaudere::work::Task task;
        try {
            if (predecessor->kind == local_goose_dialogue_v2_task_kind) {
                task = make_local_goose_dialogue_v3_bridge_from_v2_task(
                    command.id, command.speaker_kind, command.speaker_id,
                    command.message_kind, command.text,
                    local_goose_dialogue_model_sha256_, *predecessor);
            } else {
                task = make_local_goose_dialogue_v3_successor_task(
                    command.id, command.speaker_kind, command.speaker_id,
                    command.message_kind, command.text,
                    local_goose_dialogue_model_sha256_, *predecessor);
            }
        } catch (const std::invalid_argument& error) {
            return LiveControlReply{
                false, 4,
                std::string("gaudere-agent: preferred dialogue predecessor rejected: ")
                    + error.what() + "\n"};
        }

        const auto existing =
            store_.find_by_idempotency_key(task.idempotency_key);
        if (existing && !same_local_goose_dialogue_definition(*existing, task)) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue request id conflicts with an existing Task\n"};
        }

        const auto submit = runtime_.submit(task);
        if (submit != gaudere::work::SubmitResult::accepted
            && submit != gaudere::work::SubmitResult::duplicate) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: preferred Local Goose dialogue v3 submission rejected\n"};
        }

        auto stored = store_.find(task.id);
        if (!stored
            || !same_local_goose_dialogue_definition(*stored, task)) {
            throw std::runtime_error(
                "preferred Local Goose dialogue v3 Task differs after submission");
        }
        if (!gaudere::work::is_terminal(stored->status)) {
            work_may_be_pending = true;
        }

        auto replacement = *head;
        replacement.revision = head->revision + 1;
        replacement.head_task_id = task.id;
        const auto write =
            dialogue_thread_store_->replace(*head, replacement);
        switch (write.result) {
        case LocalGooseDialogueThreadStoreResult::accepted:
        case LocalGooseDialogueThreadStoreResult::duplicate:
            return LiveControlReply{
                true, 0,
                task_report(*stored)
                    + local_goose_dialogue_thread_head_report(*write.head)};
        case LocalGooseDialogueThreadStoreResult::conflict:
            return LiveControlReply{
                false, 4,
                std::string(
                    "gaudere-agent: preferred dialogue head conflict after durable Task submission\n")
                    + task_report(*stored)
                    + (write.head
                        ? local_goose_dialogue_thread_head_report(*write.head)
                        : std::string{})};
        case LocalGooseDialogueThreadStoreResult::invalid:
        case LocalGooseDialogueThreadStoreResult::unavailable:
            return LiveControlReply{
                false, 4,
                std::string(
                    "gaudere-agent: preferred dialogue head update failed after durable Task submission: ")
                    + write.detail + "\n"
                    + task_report(*stored)};
        }
    }

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

    if (command.operation == LiveControlOperation::inspect_wake_status) {
        if (!explicit_wake_) {
            return wake_disabled();
        }
        if (!scheduler_next_) {
            throw std::runtime_error(
                "wake status scheduler observation is unavailable");
        }
        const auto status = explicit_wake_->inspect_status(
            runtime_.next_recovery_at(), scheduler_next_());
        return LiveControlReply{status.healthy, status.healthy ? 0 : 4,
                                status.report};
    }

    if (command.operation == LiveControlOperation::inspect_wake) {
        if (!explicit_wake_) {
            return wake_disabled();
        }
        const auto wake = explicit_wake_->find(command.id);
        return wake
            ? LiveControlReply{true, 0, wake_intent_report(*wake)}
            : wake_not_found();
    }

    if (command.operation == LiveControlOperation::accept_wake) {
        if (!explicit_wake_) {
            return wake_disabled();
        }
        const auto acceptance = explicit_wake_->accept(command.id);
        if ((acceptance.result == ExplicitWakeAcceptResult::accepted
             || acceptance.result == ExplicitWakeAcceptResult::duplicate)
            && acceptance.intent) {
            wake_deadline_may_have_changed = true;
            return LiveControlReply{
                true, 0,
                "acceptance=" + wake_acceptance_name(acceptance.result) + "\n"
                    + wake_intent_report(*acceptance.intent)};
        }
        const int code =
            acceptance.result == ExplicitWakeAcceptResult::source_not_found
            ? 3 : 4;
        return LiveControlReply{
            false, code,
            "gaudere-agent: explicit wake acceptance="
                + wake_acceptance_name(acceptance.result) + ": "
                + acceptance.detail + "\n"};
    }

    if (command.operation == LiveControlOperation::revoke_wake) {
        if (!explicit_wake_) {
            return wake_disabled();
        }
        using Result = gaudere::scheduling::wake::WakeIntentRevokeResult;
        const auto result = explicit_wake_->revoke(command.id, command.text);
        if (result == Result::not_found) {
            return wake_not_found();
        }
        const auto wake = explicit_wake_->find(command.id);
        if (result == Result::revoked || result == Result::fired
            || result == Result::manual_review) {
            if (!wake) {
                throw std::runtime_error(
                    "terminal explicit wake is missing from durable state");
            }
            wake_deadline_may_have_changed = true;
            return LiveControlReply{
                true, 0,
                "revocation=" + wake_revoke_name(result) + "\n"
                    + wake_intent_report(*wake)};
        }
        std::string body = "gaudere-agent: wake revocation="
            + wake_revoke_name(result) + "\n";
        if (wake) {
            body += wake_intent_report(*wake);
        }
        return LiveControlReply{false, 4, std::move(body)};
    }

    if (command.operation
        == LiveControlOperation::stimulate_local_goose_cycle) {
        if (!local_goose_stimulus_) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose stimulus capability is not enabled in this service\n"};
        }
        // The callback runs synchronously on this sole owner worker. It may have
        // durably accepted or reconciled a stimulus before returning or throwing,
        // so force the caller to re-read/re-arm the cycle conservatively.
        local_goose_cycle_may_have_changed = true;
        return local_goose_stimulus_(command.id);
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
    case LiveControlOperation::submit_local_goose_dialogue:
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        task = make_local_goose_dialogue_task(
            command.id, command.text, local_goose_dialogue_model_sha256_);
        description = "Local Goose dialogue";
        break;
    case LiveControlOperation::submit_local_goose_dialogue_v2_root:
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        task = make_local_goose_dialogue_v2_root_task(
            command.id, command.text, local_goose_dialogue_model_sha256_);
        description = "Local Goose dialogue v2 root";
        break;
    case LiveControlOperation::submit_local_goose_dialogue_v2_next: {
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        const auto predecessor = store_.find(command.predecessor_task_id);
        if (!predecessor) {
            return LiveControlReply{
                false, 3,
                "gaudere-agent: Local Goose dialogue v2 predecessor Task not found\n"};
        }
        try {
            task = make_local_goose_dialogue_v2_successor_task(
                command.id, command.text,
                local_goose_dialogue_model_sha256_, *predecessor);
        } catch (const std::invalid_argument& error) {
            return LiveControlReply{
                false, 4,
                std::string("gaudere-agent: Local Goose dialogue v2 predecessor rejected: ")
                    + error.what() + "\n"};
        }
        description = "Local Goose dialogue v2 successor";
        break;
    }
    case LiveControlOperation::submit_local_goose_dialogue_v3_root:
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        try {
            task = make_local_goose_dialogue_v3_root_task(
                command.id, command.speaker_kind, command.speaker_id,
                command.message_kind, command.text,
                local_goose_dialogue_model_sha256_);
        } catch (const std::invalid_argument& error) {
            return LiveControlReply{
                false, 4,
                std::string("gaudere-agent: Local Goose dialogue v3 request rejected: ")
                    + error.what() + "\n"};
        }
        description = "Local Goose dialogue v3 root";
        break;
    case LiveControlOperation::submit_local_goose_dialogue_v3_next: {
        if (local_goose_dialogue_model_sha256_.empty()) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue capability is not enabled in this service\n"};
        }
        const auto predecessor = store_.find(command.predecessor_task_id);
        if (!predecessor) {
            return LiveControlReply{
                false, 3,
                "gaudere-agent: Local Goose dialogue v3 predecessor Task not found\n"};
        }
        try {
            if (predecessor->kind == local_goose_dialogue_v2_task_kind) {
                task = make_local_goose_dialogue_v3_bridge_from_v2_task(
                    command.id, command.speaker_kind, command.speaker_id,
                    command.message_kind, command.text,
                    local_goose_dialogue_model_sha256_, *predecessor);
            } else {
                task = make_local_goose_dialogue_v3_successor_task(
                    command.id, command.speaker_kind, command.speaker_id,
                    command.message_kind, command.text,
                    local_goose_dialogue_model_sha256_, *predecessor);
            }
        } catch (const std::invalid_argument& error) {
            return LiveControlReply{
                false, 4,
                std::string("gaudere-agent: Local Goose dialogue v3 predecessor rejected: ")
                    + error.what() + "\n"};
        }
        description = "Local Goose dialogue v3 successor";
        break;
    }
    case LiveControlOperation::bind_local_goose_dialogue_thread_head:
    case LiveControlOperation::inspect_local_goose_dialogue_thread_head:
    case LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next:
    case LiveControlOperation::inspect_task:
    case LiveControlOperation::inspect_budget:
    case LiveControlOperation::accept_wake:
    case LiveControlOperation::revoke_wake:
    case LiveControlOperation::inspect_wake:
    case LiveControlOperation::inspect_wake_status:
    case LiveControlOperation::stimulate_local_goose_cycle:
        throw std::logic_error(
            "non-submit operation unexpectedly reached submit path");
    }

    const auto id = task.id;
    const bool local_dialogue_submission =
        command.operation == LiveControlOperation::submit_local_goose_dialogue
        || command.operation == LiveControlOperation::submit_local_goose_dialogue_v2_root
        || command.operation == LiveControlOperation::submit_local_goose_dialogue_v2_next
        || command.operation == LiveControlOperation::submit_local_goose_dialogue_v3_root
        || command.operation == LiveControlOperation::submit_local_goose_dialogue_v3_next;
    if (local_dialogue_submission) {
        const auto existing = store_.find_by_idempotency_key(task.idempotency_key);
        if (existing && !same_local_goose_dialogue_definition(*existing, task)) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: Local Goose dialogue request id conflicts with an existing Task\n"};
        }
    }
    const auto submit = runtime_.submit(task);
    if (submit != gaudere::work::SubmitResult::accepted
        && submit != gaudere::work::SubmitResult::duplicate) {
        return LiveControlReply{
            false, 4, "gaudere-agent: " + description + " submission rejected\n"};
    }

    auto stored = store_.find(id);
    if (!stored) {
        throw std::runtime_error(description + " task is missing after submission");
    }
    if (local_dialogue_submission
        && !same_local_goose_dialogue_definition(*stored, task)) {
        return LiveControlReply{
            false, 4,
            "gaudere-agent: Local Goose dialogue request conflicts with an existing Task\n"};
    }

    if (goose_synchronous_echo(command)) {
        if (!same_local_echo_definition(*stored, task)) {
            return LiveControlReply{
                false, 4,
                "gaudere-agent: reserved Goose local echo id conflicts with an existing Task\n"};
        }
        if (!gaudere::work::is_terminal(stored->status)) {
            LocalEchoHandler echo_handler;
            TaskExecutor executor(runtime_, store_);
            const auto executed = executor.execute(
                id, "local-goose-tool", echo_handler);
            if (executed != ExecuteResult::completed) {
                stored = store_.find(id);
                return LiveControlReply{
                    false, 4,
                    std::string("gaudere-agent: reserved Goose local echo could not execute synchronously\n")
                        + (stored ? task_report(*stored) : std::string{})};
            }
            stored = store_.find(id);
            if (!stored) {
                throw std::runtime_error(
                    "synchronous Goose local echo disappeared after execution");
            }
        }
        if (stored->status != gaudere::work::TaskStatus::succeeded) {
            return LiveControlReply{false, 4, task_report(*stored)};
        }
        work_may_be_pending = false;
        return LiveControlReply{true, 0, task_report(*stored)};
    }

    if (!gaudere::work::is_terminal(stored->status)) {
        work_may_be_pending = true;
    }
    return LiveControlReply{true, 0, task_report(*stored)};
}

} // namespace gaudere_agent
