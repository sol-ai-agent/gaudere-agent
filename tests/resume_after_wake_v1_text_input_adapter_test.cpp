#include "ResumeAfterWakeV1.hpp"
#include "ResumeAfterWakeV1TextInputAdapter.hpp"

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using gaudere_agent::HandlerOutcome;
using gaudere_agent::HandlerResult;
using gaudere_agent::TaskContext;
using gaudere_agent::TaskHandler;
using Task = gaudere::work::Task;

void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool same_limits(const gaudere::work::ResourceLimits& a,
                 const gaudere::work::ResourceLimits& b)
{
    return a.max_input_bytes == b.max_input_bytes
        && a.max_output_bytes == b.max_output_bytes
        && a.max_runtime == b.max_runtime
        && a.max_attempts == b.max_attempts;
}

class RecordingHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        ++calls;
        seen = context.task;
        cancellation_seen = context.cancellation_requested
            && context.cancellation_requested();
        return HandlerResult{
            HandlerOutcome::succeeded,
            "application/json",
            "{\"decision\":\"stop\",\"reason\":\"adapter proof\",\"schema\":\"gaudere.cognition.resume-decision.v1\"}",
            {}, {}};
    }

    int calls = 0;
    std::optional<Task> seen;
    bool cancellation_seen = false;
};

Task sample_task()
{
    Task task;
    task.id = std::string{gaudere_agent::resume_after_wake_v1_task_prefix}
        + "source-1";
    task.idempotency_key = task.id;
    task.kind = gaudere_agent::resume_after_wake_v1_task_kind;
    task.input_content_type = gaudere_agent::resume_after_wake_v1_content_type;
    task.input = "{\"canonical\":\"bytes stay identical\"}";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = std::chrono::seconds{60};
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::running;
    task.lease = gaudere::work::Lease{
        "worker-proof",
        std::chrono::system_clock::time_point{std::chrono::milliseconds{123456}}};
    task.cancel_reason = "unchanged-cancel-reason";
    return task;
}

void prove_only_content_type_changes()
{
    RecordingHandler downstream;
    gaudere_agent::ResumeAfterWakeV1TextInputAdapter adapter(downstream);
    const auto original = sample_task();

    const auto result = adapter.execute(TaskContext{original, [] { return true; }});
    require(result.outcome == HandlerOutcome::succeeded,
            "downstream result must be preserved");
    require(downstream.calls == 1 && downstream.seen.has_value(),
            "valid v1 task must reach downstream exactly once");
    require(downstream.cancellation_seen,
            "cancellation probe must be preserved");

    const auto& seen = *downstream.seen;
    require(seen.input_content_type == "text/plain; charset=utf-8",
            "adapter must expose text/plain to provider boundary");
    require(seen.id == original.id
            && seen.idempotency_key == original.idempotency_key
            && seen.kind == original.kind
            && seen.input == original.input
            && same_limits(seen.limits, original.limits)
            && seen.attempts_started == original.attempts_started
            && seen.status == original.status
            && seen.cancel_reason == original.cancel_reason,
            "adapter must preserve durable task identity and definition");
    require(seen.lease.has_value() && original.lease.has_value()
            && seen.lease->owner == original.lease->owner
            && seen.lease->expires_at == original.lease->expires_at,
            "adapter must preserve lease evidence in transient copy");
    require(!seen.result.has_value() && !original.result.has_value(),
            "adapter must preserve result state");
    require(original.input_content_type == gaudere_agent::resume_after_wake_v1_content_type,
            "adapter must not mutate durable/original task object");
}

void prove_wrong_shape_fails_before_downstream()
{
    RecordingHandler downstream;
    gaudere_agent::ResumeAfterWakeV1TextInputAdapter adapter(downstream);

    auto wrong_kind = sample_task();
    wrong_kind.kind = "local.echo";
    auto result = adapter.execute(TaskContext{wrong_kind, {}});
    require(result.outcome == HandlerOutcome::failed
            && result.failure_code == "cognition_invalid_resume_provider_input"
            && downstream.calls == 0,
            "wrong kind must fail before provider boundary");

    auto wrong_type = sample_task();
    wrong_type.input_content_type = "text/plain; charset=utf-8";
    result = adapter.execute(TaskContext{wrong_type, {}});
    require(result.outcome == HandlerOutcome::failed
            && result.failure_code == "cognition_invalid_resume_provider_input"
            && downstream.calls == 0,
            "already-adapted or wrong durable type must fail closed");
}

} // namespace

int main()
{
    try {
        prove_only_content_type_changes();
        prove_wrong_shape_fails_before_downstream();
        std::cout << "resume-after-wake v1 text input adapter: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "resume-after-wake v1 text input adapter: FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
