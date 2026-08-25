#include "ResumeAfterWakeV1Prepare.hpp"

#include "TaskExecutor.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

constexpr std::size_t max_request_bytes = 32 * 1024;
constexpr std::size_t max_capsule_bytes = 24 * 1024;
constexpr std::uint64_t selection_max_bytes = 64 * 1024;

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

bool same_limits(const gaudere::work::ResourceLimits& a,
                 const gaudere::work::ResourceLimits& b) noexcept
{
    return a.max_input_bytes == b.max_input_bytes
        && a.max_output_bytes == b.max_output_bytes
        && a.max_runtime == b.max_runtime
        && a.max_attempts == b.max_attempts;
}

bool same_definition(const Task& a, const Task& b) noexcept
{
    return a.id == b.id && a.idempotency_key == b.idempotency_key
        && a.kind == b.kind && a.input_content_type == b.input_content_type
        && a.input == b.input && same_limits(a.limits, b.limits);
}

Task make_selection_task(const std::string& wake_id,
                         const std::string& request_json,
                         const gaudere::work::TimePoint captured_at)
{
    if (wake_id.empty()) {
        throw std::invalid_argument("resume v1 selection wake id is empty");
    }
    if (request_json.empty() || request_json.size() > max_request_bytes) {
        throw std::invalid_argument(
            "resume v1 context request must be 1..32768 bytes");
    }
    const auto captured = milliseconds(captured_at);
    if (captured < 0) {
        throw std::invalid_argument(
            "resume v1 selection capture time precedes Unix epoch");
    }

    const Json input = {
        {"schema", resume_after_wake_v1_selection_schema},
        {"wake_id", wake_id},
        {"captured_at_ms", captured},
        {"request_json", request_json}
    };

    Task task;
    task.id = std::string{resume_after_wake_v1_selection_task_prefix} + wake_id;
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_v1_selection_task_kind;
    task.input_content_type = resume_after_wake_v1_selection_content_type;
    task.input = input.dump();
    task.limits.max_input_bytes = selection_max_bytes;
    task.limits.max_output_bytes = selection_max_bytes;
    task.limits.max_runtime = std::chrono::seconds{2};
    task.limits.max_attempts = 2;
    if (task.input.size() > task.limits.max_input_bytes) {
        throw std::invalid_argument("resume v1 selection exceeds hard bounds");
    }
    return task;
}

struct SelectionInspection {
    bool eligible = false;
    gaudere::work::TimePoint captured_at{};
    std::string request_json;
    std::string detail;
};

SelectionInspection inspect_selection(const Task& task,
                                      const std::string& wake_id) noexcept
{
    try {
        const auto expected_id =
            std::string{resume_after_wake_v1_selection_task_prefix} + wake_id;
        if (task.id != expected_id || task.idempotency_key != expected_id
            || task.kind != resume_after_wake_v1_selection_task_kind
            || task.input_content_type != resume_after_wake_v1_selection_content_type
            || task.limits.max_input_bytes != selection_max_bytes
            || task.limits.max_output_bytes != selection_max_bytes
            || task.limits.max_runtime != std::chrono::seconds{2}
            || task.limits.max_attempts != 2
            || task.input.empty() || task.input.size() > selection_max_bytes) {
            return {false, {}, {}, "resume v1 selection Task shape conflicts"};
        }

        const auto input = Json::parse(task.input);
        static const std::set<std::string> keys = {
            "schema", "wake_id", "captured_at_ms", "request_json"
        };
        if (!input.is_object() || input.size() != keys.size()) {
            return {false, {}, {}, "resume v1 selection input is not exact"};
        }
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (keys.find(it.key()) == keys.end()) {
                return {false, {}, {}, "resume v1 selection has unknown key"};
            }
        }
        if (!input.at("schema").is_string()
            || input.at("schema").get<std::string>()
                != resume_after_wake_v1_selection_schema
            || !input.at("wake_id").is_string()
            || input.at("wake_id").get<std::string>() != wake_id
            || !(input.at("captured_at_ms").is_number_integer()
                 || input.at("captured_at_ms").is_number_unsigned())
            || !input.at("request_json").is_string()) {
            return {false, {}, {}, "resume v1 selection input schema conflicts"};
        }
        const auto captured = input.at("captured_at_ms").get<std::int64_t>();
        const auto request = input.at("request_json").get<std::string>();
        if (captured < 0 || request.empty() || request.size() > max_request_bytes) {
            return {false, {}, {}, "resume v1 selection values are outside bounds"};
        }
        const auto expected = make_selection_task(
            wake_id, request,
            gaudere::work::TimePoint{std::chrono::milliseconds{captured}});
        if (!same_definition(task, expected)) {
            return {false, {}, {}, "resume v1 selection is not canonical"};
        }
        if (task.status == TaskStatus::succeeded) {
            if (!task.result
                || task.result->content_type
                    != resume_after_wake_v1_selection_content_type
                || task.result->output != task.input
                || !task.result->failure_code.empty()
                || !task.result->failure_message.empty()) {
                return {false, {}, {},
                        "terminal resume v1 selection result conflicts"};
            }
        }
        return {true,
                gaudere::work::TimePoint{std::chrono::milliseconds{captured}},
                request,
                {}};
    } catch (...) {
        return {false, {}, {}, "resume v1 selection cannot be parsed canonically"};
    }
}

class SelectionHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        return HandlerResult{HandlerOutcome::succeeded,
                             resume_after_wake_v1_selection_content_type,
                             context.task.input, {}, {}};
    }
};

struct ReadySelection {
    bool ready = false;
    bool duplicate = false;
    std::optional<Task> task;
    gaudere::work::TimePoint captured_at{};
    std::string detail;
};

ReadySelection finish_selection(gaudere::work::TaskStore& store,
                                gaudere::work::Runtime& runtime,
                                const std::string& wake_id,
                                const std::string& request_json,
                                const gaudere::work::TimePoint now)
{
    const auto id = std::string{resume_after_wake_v1_selection_task_prefix} + wake_id;
    auto by_id = store.find(id);
    auto by_key = store.find_by_idempotency_key(id);
    if (by_id && by_key && by_id->id != by_key->id) {
        return {false, false, {}, {},
                "resume v1 selection id/key resolve to different Tasks"};
    }

    bool duplicate = static_cast<bool>(by_id || by_key);
    auto existing = by_id ? by_id : by_key;
    if (!existing) {
        Task candidate;
        try {
            candidate = make_selection_task(wake_id, request_json, now);
        } catch (const std::exception& error) {
            return {false, false, {}, {}, error.what()};
        }
        const auto submitted = runtime.submit(candidate);
        if (submitted == gaudere::work::SubmitResult::invalid
            || submitted == gaudere::work::SubmitResult::unavailable) {
            return {false, false, {}, {},
                    "resume v1 selection submission was rejected"};
        }
        duplicate = submitted == gaudere::work::SubmitResult::duplicate;
        existing = store.find(id);
        by_key = store.find_by_idempotency_key(id);
        if (!existing || !by_key || existing->id != by_key->id) {
            return {false, duplicate, existing, {},
                    "durable resume v1 selection is missing after submit"};
        }
    }

    auto inspected = inspect_selection(*existing, wake_id);
    if (!inspected.eligible) {
        return {false, duplicate, existing, {}, inspected.detail};
    }
    if (inspected.request_json != request_json) {
        return {false, true, existing, inspected.captured_at,
                "resume v1 selection already froze different request bytes"};
    }

    if (existing->status == TaskStatus::pending) {
        SelectionHandler handler;
        TaskExecutor executor(runtime, store);
        if (executor.execute(existing->id, "resume-v1-selection", handler)
            != ExecuteResult::completed) {
            return {false, duplicate, existing, inspected.captured_at,
                    "resume v1 selection local execution did not complete"};
        }
        existing = store.find(id);
        if (!existing) {
            return {false, duplicate, {}, inspected.captured_at,
                    "resume v1 selection disappeared after local execution"};
        }
        inspected = inspect_selection(*existing, wake_id);
        if (!inspected.eligible) {
            return {false, duplicate, existing, {}, inspected.detail};
        }
    }

    if (existing->status == TaskStatus::running
        || existing->status == TaskStatus::cancel_requested) {
        return {false, true, existing, inspected.captured_at,
                "resume v1 selection still owns a live lease"};
    }
    if (existing->status != TaskStatus::succeeded) {
        return {false, duplicate, existing, inspected.captured_at,
                "resume v1 selection is terminal but not succeeded"};
    }
    inspected = inspect_selection(*existing, wake_id);
    if (!inspected.eligible) {
        return {false, duplicate, existing, {}, inspected.detail};
    }
    return {true, duplicate, existing, inspected.captured_at, {}};
}

} // namespace

ResumeAfterWakeV1Prepare::ResumeAfterWakeV1Prepare(
    gaudere::work::TaskStore& task_store,
    gaudere::scheduling::wake::WakeIntentStore& wake_store,
    gaudere::work::Runtime& work_runtime,
    Now now,
    const bool enabled,
    Progress progress)
    : task_store_(task_store), wake_store_(wake_store),
      work_runtime_(work_runtime), now_(std::move(now)), enabled_(enabled),
      progress_(std::move(progress))
{
    if (!now_) {
        throw std::invalid_argument("resume v1 preparation clock is required");
    }
}

ResumeAfterWakeV1Preparation ResumeAfterWakeV1Prepare::prepare(
    const std::string& wake_id,
    const std::string& request_json)
{
    if (!enabled_) {
        return {false, false, {}, {}, {},
                "resume-after-wake v1 preparation is disabled"};
    }
    if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
        return {false, false, {}, {}, {},
                "work runtime is not running"};
    }

    const auto request = inspect_resume_context_snapshot_request(request_json);
    if (!request.eligible) {
        return {false, false, {}, {}, {}, request.detail};
    }
    const auto capture_now = now_();
    try {
        auto prospective_capsule = Json::parse(request.canonical_request);
        prospective_capsule["captured_at_ms"] = milliseconds(capture_now);
        if (prospective_capsule.dump().size() > max_capsule_bytes) {
            return {false, false, {}, {}, {},
                    "canonical context capsule would exceed 24576 bytes"};
        }
    } catch (...) {
        return {false, false, {}, {}, {},
                "canonical context request could not be re-parsed"};
    }

    const auto selection = finish_selection(
        task_store_, work_runtime_, wake_id, request.canonical_request, capture_now);
    if (!selection.ready || !selection.task) {
        return {false, selection.duplicate, selection.task, {}, {}, selection.detail};
    }
    if (progress_) progress_("selection_durable");

    // The durable selection freezes the original capture clock. Reconstructing
    // the recorder with that clock makes retry after any later crash produce the
    // exact same content-addressed snapshot identity instead of a false new memory.
    ResumeContextSnapshotRecorder recorder(
        task_store_, work_runtime_, [captured = selection.captured_at] {
            return captured;
        });
    const auto snapshot = recorder.record(request.canonical_request);
    if ((snapshot.result != ResumeContextSnapshotRecordResult::accepted
         && snapshot.result != ResumeContextSnapshotRecordResult::duplicate)
        || !snapshot.task) {
        return {false, selection.duplicate, selection.task, snapshot.task, {},
                snapshot.detail.empty()
                    ? "resume v1 snapshot recording failed"
                    : snapshot.detail};
    }
    if (progress_) progress_("snapshot_durable");

    ResumeAfterWakeV1 resume(
        task_store_, wake_store_, work_runtime_, now_, true);
    auto claim = resume.claim(wake_id, snapshot.task->id);
    if (claim.result != ResumeAfterWakeV1ClaimResult::accepted
        && claim.result != ResumeAfterWakeV1ClaimResult::duplicate) {
        return {false,
                selection.duplicate
                    || snapshot.result == ResumeContextSnapshotRecordResult::duplicate,
                selection.task, snapshot.task, claim,
                claim.detail.empty() ? "resume v1 claim failed" : claim.detail};
    }
    if (progress_) progress_("claim_durable");

    const bool duplicate = selection.duplicate
        || snapshot.result == ResumeContextSnapshotRecordResult::duplicate
        || claim.result == ResumeAfterWakeV1ClaimResult::duplicate;
    return {true, duplicate, selection.task, snapshot.task, std::move(claim), {}};
}

} // namespace gaudere_agent
