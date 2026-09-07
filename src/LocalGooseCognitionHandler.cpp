#include "LocalGooseCognitionHandler.hpp"

#include "LocalGooseCognition.hpp"

#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

std::string governed_prompt(const LocalGooseCognitionInspection& cognition,
                            const bool tools_enabled)
{
    auto prompt = local_goose_prompt(cognition);
    if (!tools_enabled) return prompt;

    const std::string legacy =
        "In this local gate you have no tools, network, secrets, shell, or external-action authority. ";
    const std::string governed =
        "In this local gate you have no arbitrary shell, direct network, direct secret access, or host authority. "
        "You may use only the typed Gaudere tools currently exposed by your own durable operational policy. "
        "You may change that operational tool policy within the current risk envelope. "
        "You may propose changes to the risk envelope, but local Goose cognition cannot approve or promote them; "
        "a separate OpenAI-validated Gaudere governance path is required for any such promotion. ";
    const auto position = prompt.find(legacy);
    if (position == std::string::npos) {
        throw std::logic_error("local Goose base prompt boundary sentence changed unexpectedly");
    }
    prompt.replace(position, legacy.size(), governed);
    return prompt;
}

} // namespace

LocalGooseCognitionHandler::LocalGooseCognitionHandler(
    LocalGooseRunner& runner,
    std::string model_path,
    std::string model_sha256,
    const bool tools_enabled,
    std::string control_socket,
    std::string governance_path)
    : runner_(runner), model_path_(std::move(model_path)),
      model_sha256_(std::move(model_sha256)), tools_enabled_(tools_enabled),
      control_socket_(std::move(control_socket)),
      governance_path_(std::move(governance_path))
{
    if (tools_enabled_ && (control_socket_.empty() || governance_path_.empty())) {
        throw std::invalid_argument(
            "typed local Goose tools require control socket and governance sidecar paths");
    }
    if (!tools_enabled_ && (!control_socket_.empty() || !governance_path_.empty())) {
        throw std::invalid_argument(
            "local Goose tool paths require typed tools to be enabled");
    }
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
    request.prompt = governed_prompt(cognition, tools_enabled_);
    request.timeout = context.task.limits.max_runtime;
    request.max_output_bytes = static_cast<std::size_t>(
        context.task.limits.max_output_bytes);
    request.tools_enabled = tools_enabled_;
    request.control_socket = control_socket_;
    request.governance_path = governance_path_;
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
