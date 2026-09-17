#include "LocalGooseCycleHandler.hpp"

#include "LocalGooseCycle.hpp"

#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

std::string normalize_model_selector(std::string value)
{
    if (!value.empty() && value.front() == '/') value.erase(0, 1);
    return value;
}

} // namespace

LocalGooseCycleHandler::LocalGooseCycleHandler(
    LocalGooseRunner& runner,
    std::string model_id,
    std::string model_sha256,
    const bool tools_enabled,
    std::string control_socket,
    std::string governance_path,
    std::string goose_path_root)
    : runner_(runner), model_id_(normalize_model_selector(std::move(model_id))),
      model_sha256_(std::move(model_sha256)), tools_enabled_(tools_enabled),
      control_socket_(std::move(control_socket)),
      governance_path_(std::move(governance_path)),
      goose_path_root_(std::move(goose_path_root))
{
    if (model_id_.empty() || goose_path_root_.empty())
        throw std::invalid_argument("local Goose cycle model id/path root must not be empty");
    if (tools_enabled_ && (control_socket_.empty() || governance_path_.empty()))
        throw std::invalid_argument(
            "typed local Goose cycle tools require control socket and governance paths");
    if (!tools_enabled_ && (!control_socket_.empty() || !governance_path_.empty()))
        throw std::invalid_argument(
            "local Goose cycle tool paths require typed tools to be enabled");
}

HandlerResult LocalGooseCycleHandler::execute(const TaskContext& context)
{
    if (context.cancellation_requested && context.cancellation_requested())
        return {HandlerOutcome::cancelled, {}, {}, {}, {}};

    const auto cycle = inspect_local_goose_cycle_task(context.task);
    if (!cycle.eligible || cycle.model_sha256 != model_sha256_) {
        return {HandlerOutcome::failed, {}, {}, "invalid_local_goose_cycle",
                cycle.detail.empty() ? "local Goose cycle/model is not canonical"
                                     : cycle.detail};
    }

    LocalGooseRunRequest request;
    request.model_id = model_id_;
    request.goose_path_root = goose_path_root_;
    request.prompt = local_goose_cycle_prompt(cycle);
    request.timeout = context.task.limits.max_runtime;
    request.max_output_bytes = static_cast<std::size_t>(
        context.task.limits.max_output_bytes);
    request.tools_enabled = tools_enabled_;
    request.control_socket = control_socket_;
    request.governance_path = governance_path_;

    const auto run = runner_.run(request);
    if (run.outcome != LocalGooseRunOutcome::succeeded) {
        const char* code = run.outcome == LocalGooseRunOutcome::timed_out
            ? "local_goose_cycle_timeout"
            : run.outcome == LocalGooseRunOutcome::output_too_large
                ? "local_goose_cycle_output_too_large"
                : "local_goose_cycle_failed";
        return {HandlerOutcome::failed, {}, {}, code,
                run.detail.empty() ? "local Goose cycle inference failed" : run.detail};
    }

    const auto decision = inspect_local_goose_decision(run.output);
    if (!decision.eligible) {
        return {HandlerOutcome::failed, {}, {}, "invalid_local_goose_cycle_decision",
                decision.detail.empty() ? "local Goose cycle decision is not canonical"
                                        : decision.detail};
    }
    return {HandlerOutcome::succeeded,
            local_goose_decision_content_type,
            decision.decision.canonical_json,
            {}, {}};
}

} // namespace gaudere_agent
