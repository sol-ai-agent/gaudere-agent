#include "LocalGooseDialoguePreferredSubmitter.hpp"

#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

bool same_dialogue_definition(const gaudere::work::Task& stored,
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

bool same_request(const gaudere::work::Task& task,
                  const std::string& request_id,
                  const std::string& speaker_kind,
                  const std::string& speaker_id,
                  const std::string& message_kind,
                  const std::string& message) noexcept
{
    const auto dialogue = inspect_local_goose_dialogue_v3_task(task);
    return dialogue.eligible
        && dialogue.request_id == request_id
        && dialogue.speaker_kind == speaker_kind
        && dialogue.speaker_id == speaker_id
        && dialogue.message_kind == message_kind
        && dialogue.message == message;
}

} // namespace

LocalGooseDialoguePreferredSubmitResult
submit_local_goose_dialogue_v3_preferred(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueThreadStore& thread_store,
    const std::string& model_sha256,
    const std::string& request_id,
    const std::string& thread_alias,
    const std::uint64_t expected_thread_revision,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message)
{
    LocalGooseDialoguePreferredSubmitResult out;

    if (model_sha256.empty()) {
        out.detail = "Local Goose dialogue capability is not enabled";
        return out;
    }

    auto head = thread_store.find(thread_alias);
    if (!head) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.detail = "preferred dialogue thread alias not found";
        return out;
    }

    if (head->revision != expected_thread_revision) {
        if (expected_thread_revision
                < static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            && head->revision == expected_thread_revision + 1) {
            const auto current_task = task_store.find(head->head_task_id);
            if (current_task
                && same_request(
                    *current_task, request_id, speaker_kind, speaker_id,
                    message_kind, message)) {
                out.result =
                    LocalGooseDialoguePreferredSubmitResultCode::duplicate;
                out.task = current_task;
                out.head = head;
                out.work_may_be_pending =
                    !gaudere::work::is_terminal(current_task->status);
                out.detail = "preferred dialogue request already committed";
                return out;
            }
            const auto expected_task = task_store.find_by_idempotency_key(
                std::string{local_goose_dialogue_v3_task_prefix}
                    + "request-id:" + sha256_hex(request_id));
            if (expected_task
                && same_request(
                    *expected_task, request_id, speaker_kind, speaker_id,
                    message_kind, message)) {
                out.task = expected_task;
                out.work_may_be_pending =
                    !gaudere::work::is_terminal(expected_task->status);
            }
        }
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.detail = "preferred dialogue thread revision conflict";
        return out;
    }

    if (head->revision
        == static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.detail = "preferred dialogue thread revision exhausted";
        return out;
    }

    const auto predecessor = task_store.find(head->head_task_id);
    if (!predecessor) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.detail = "preferred dialogue predecessor Task not found";
        return out;
    }

    const auto root = canonical_dialogue_root(*predecessor);
    if (!root || *root != head->root_task_id) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.detail =
            "preferred dialogue head is not canonical success for declared root";
        return out;
    }

    gaudere::work::Task task;
    try {
        if (predecessor->kind == local_goose_dialogue_v2_task_kind) {
            task = make_local_goose_dialogue_v3_bridge_from_v2_task(
                request_id, speaker_kind, speaker_id, message_kind, message,
                model_sha256, *predecessor);
        } else {
            task = make_local_goose_dialogue_v3_successor_task(
                request_id, speaker_kind, speaker_id, message_kind, message,
                model_sha256, *predecessor);
        }
    } catch (const std::invalid_argument& error) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::invalid;
        out.head = head;
        out.detail = error.what();
        return out;
    }

    const auto existing =
        task_store.find_by_idempotency_key(task.idempotency_key);
    if (existing && !same_dialogue_definition(*existing, task)) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.task = existing;
        out.detail =
            "Local Goose dialogue request id conflicts with an existing Task";
        return out;
    }

    const auto submit = runtime.submit(task);
    if (submit != gaudere::work::SubmitResult::accepted
        && submit != gaudere::work::SubmitResult::duplicate) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.head = head;
        out.detail =
            "preferred Local Goose dialogue v3 submission rejected";
        return out;
    }

    auto stored = task_store.find(task.id);
    if (!stored || !same_dialogue_definition(*stored, task)) {
        out.result = LocalGooseDialoguePreferredSubmitResultCode::unavailable;
        out.head = head;
        out.detail =
            "preferred Local Goose dialogue v3 Task differs after submission";
        return out;
    }
    out.work_may_be_pending = !gaudere::work::is_terminal(stored->status);

    auto replacement = *head;
    replacement.revision = head->revision + 1;
    replacement.head_task_id = task.id;
    const auto write = thread_store.replace(*head, replacement);
    out.task = stored;
    out.head = write.head;

    switch (write.result) {
    case LocalGooseDialogueThreadStoreResult::accepted:
        out.result = LocalGooseDialoguePreferredSubmitResultCode::accepted;
        out.detail = "preferred dialogue request committed";
        return out;
    case LocalGooseDialogueThreadStoreResult::duplicate:
        out.result = LocalGooseDialoguePreferredSubmitResultCode::duplicate;
        out.detail = "preferred dialogue request already committed";
        return out;
    case LocalGooseDialogueThreadStoreResult::conflict:
        out.result = LocalGooseDialoguePreferredSubmitResultCode::conflict;
        out.detail =
            "preferred dialogue head conflict after durable Task submission";
        return out;
    case LocalGooseDialogueThreadStoreResult::invalid:
        out.result = LocalGooseDialoguePreferredSubmitResultCode::invalid;
        out.detail = "preferred dialogue head update invalid: " + write.detail;
        return out;
    case LocalGooseDialogueThreadStoreResult::unavailable:
        out.result = LocalGooseDialoguePreferredSubmitResultCode::unavailable;
        out.detail =
            "preferred dialogue head update unavailable: " + write.detail;
        return out;
    }

    out.result = LocalGooseDialoguePreferredSubmitResultCode::unavailable;
    out.detail = "unknown preferred dialogue head update result";
    return out;
}

} // namespace gaudere_agent
