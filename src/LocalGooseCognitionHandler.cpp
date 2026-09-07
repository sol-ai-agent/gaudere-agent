#include "LocalGooseCognitionHandler.hpp"

#include "LocalGooseCognition.hpp"

#include <utility>

namespace gaudere_agent {

LocalGooseCognitionHandler::LocalGooseCognitionHandler(
    LocalGooseRunner& runner, std::string model_path, std::string model_sha256)
    : runner_(runner), model_path_(std::move(model_path)),
      model_sha256_(std::move(model_sha256))
{
}

HandlerResult LocalGooseCognitionHandler::execute(const TaskContext& context)
{
    if (context.cancellation_requested && context.cancellation_requested())
        return {HandlerOutcome::cancelled, {}, {}, {}, {}};

    const auto cognition = inspect_local_goose_cognition_task(context.task);
    if (!cognition.eligible || cognition.model_sha256 != model_sha256_) {
        return {HandlerOutcome::failed, {}, {}, "invalid_local_goose_cognition",
                cognition.detail.empty() ? "local Goose cognition/model is not canonical"
                                         : cognition.detail};
    }

    LocalGooseRunRequest request;
    request.model_path = model_path_;
    request.prompt = local_goose_prompt(cognition);
    request.timeout = context.task.limits.max_runtime;
    request.max_output_bytes = static_cast<std::size_t>(
        context.task.limits.max_output_bytes);
    const auto run = runner_.run(request);
    if (run.outcome != LocalGooseRunOutcome::succeeded) {
        const char* code = run.outcome == LocalGooseRunOutcome::timed_out
            ? "local_goose_timeout"
            : run.outcome == LocalGooseRunOutcome::output_too_large
                ? "local_goose_output_too_large"
                : "local_goose_failed";
        return {HandlerOutcome::failed, {}, {}, code,
                run.detail.empty() ? "local Goose inference failed" : run.detail};
    }

    const auto decision = inspect_local_goose_decision(run.output);
    if (!decision.eligible) {
        return {HandlerOutcome::failed, {}, {}, "invalid_local_goose_decision",
                decision.detail.empty() ? "local Goose decision is not canonical"
                                        : decision.detail};
    }
    return {HandlerOutcome::succeeded,
            local_goose_decision_content_type,
            decision.decision.canonical_json,
            {}, {}};
}

} // namespace gaudere_agent
