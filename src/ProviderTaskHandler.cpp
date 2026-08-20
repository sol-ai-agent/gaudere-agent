#include "ProviderTaskHandler.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using gaudere::scheduling::wake::Action;
using gaudere::scheduling::wake::ActionStatus;
using gaudere::scheduling::wake::EffectResult;
using gaudere::scheduling::wake::SubmitResult;

} // namespace

ProviderTaskHandler::ProviderTaskHandler(
    gaudere::scheduling::wake::Runtime& action_runtime,
    gaudere::scheduling::wake::ActionStore& action_store,
    Provider& provider,
    gaudere::budget::Store& budget_store,
    gaudere::budget::Policy budget_policy,
    BudgetNow budget_now)
    : action_runtime_(action_runtime),
      action_store_(action_store),
      provider_(provider),
      budget_store_(budget_store),
      budget_policy_(std::move(budget_policy)),
      budget_now_(std::move(budget_now))
{
    if (provider_.name().empty()) {
        throw std::invalid_argument("provider name must not be empty");
    }
    if (!gaudere::budget::valid_policy(budget_policy_)) {
        throw std::invalid_argument("provider budget policy is invalid");
    }
    if (!budget_now_) {
        throw std::invalid_argument("provider budget clock is required");
    }
}

std::string ProviderTaskHandler::action_id(const gaudere::work::Task& task) const
{
    return "provider.call:" + std::string(provider_.name()) + ":" + task.id;
}

std::string ProviderTaskHandler::action_key(const gaudere::work::Task& task) const
{
    return "provider.call:" + std::string(provider_.name()) + ":"
        + task.idempotency_key;
}

std::string ProviderTaskHandler::budget_scope() const
{
    return "provider.call:" + std::string(provider_.name());
}

HandlerResult ProviderTaskHandler::manual_review(std::string code,
                                                 std::string message) const
{
    return HandlerResult{HandlerOutcome::manual_review, {}, {},
                         std::move(code), std::move(message)};
}

HandlerResult ProviderTaskHandler::budget_denied(
    const gaudere::budget::ConsumeResult result) const
{
    switch (result) {
    case gaudere::budget::ConsumeResult::total_exhausted:
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "provider_budget_total_exhausted",
                             "provider lifetime call budget is exhausted"};
    case gaudere::budget::ConsumeResult::window_exhausted:
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "provider_budget_window_exhausted",
                             "provider rolling-window call budget is exhausted"};
    case gaudere::budget::ConsumeResult::cooldown:
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "provider_budget_cooldown",
                             "provider minimum call interval has not elapsed"};
    case gaudere::budget::ConsumeResult::clock_rollback:
        return manual_review(
            "provider_budget_clock_rollback",
            "system clock moved backwards relative to the durable provider budget");
    case gaudere::budget::ConsumeResult::accepted:
    case gaudere::budget::ConsumeResult::duplicate:
        break;
    }
    return manual_review("provider_budget_state_conflict",
                         "provider budget returned an invalid admission result");
}

HandlerResult ProviderTaskHandler::existing_action_result(const Action& action)
{
    if (action.status == ActionStatus::succeeded
        || action.effect_result == EffectResult::confirmed) {
        return manual_review(
            "provider_response_not_durable",
            "provider call was already confirmed but its response is not available in the task result");
    }

    if (action.status == ActionStatus::manual_review
        || action.effect_result == EffectResult::unknown) {
        if (action.status == ActionStatus::running
            && action.effect_result == EffectResult::unknown) {
            static_cast<void>(action_runtime_.record_unknown_result(action.id));
        }
        return manual_review(
            "provider_effect_unknown",
            "a previous provider attempt may have produced an external effect; automatic replay is forbidden");
    }

    // The first provider slice deliberately never replays an existing action, even
    // if it appears not to have crossed the effect boundary. This sacrifices some
    // automatic recovery in exchange for a simple no-duplicate-call invariant.
    if (action.status == ActionStatus::running) {
        static_cast<void>(action_runtime_.transition(action.id, ActionStatus::manual_review));
    } else if (action.status == ActionStatus::pending
               || action.status == ActionStatus::retry_wait) {
        static_cast<void>(action_runtime_.transition(action.id, ActionStatus::failed_permanent));
    }
    return manual_review(
        "provider_action_already_exists",
        "a previous provider action exists without a durable task response; operator review is required");
}

HandlerResult ProviderTaskHandler::execute(const TaskContext& context)
{
    if (context.cancellation_requested()) {
        return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
    }

    const auto key = action_key(context.task);
    if (const auto existing = action_store_.find_by_idempotency_key(key)) {
        return existing_action_result(*existing);
    }

    gaudere::budget::ConsumeResult budget_result;
    try {
        budget_result = budget_store_.consume(
            budget_scope(), key, budget_now_(), budget_policy_);
    } catch (...) {
        return manual_review(
            "provider_budget_unavailable",
            "provider budget could not be checked and persisted before invocation");
    }
    if (budget_result != gaudere::budget::ConsumeResult::accepted
        && budget_result != gaudere::budget::ConsumeResult::duplicate) {
        return budget_denied(budget_result);
    }

    Action action;
    action.id = action_id(context.task);
    action.idempotency_key = key;
    action.critical = true;

    const auto submitted = action_runtime_.submit(action);
    if (submitted == SubmitResult::duplicate) {
        const auto existing = action_store_.find_by_idempotency_key(key);
        return existing
            ? existing_action_result(*existing)
            : manual_review("provider_action_state_conflict",
                            "provider action became duplicate but cannot be reloaded");
    }
    if (submitted != SubmitResult::accepted) {
        return manual_review("provider_action_unavailable",
                             "provider action cannot be submitted in the current runtime state");
    }

    if (!action_runtime_.start(action.id, "provider-handler",
                               context.task.limits.max_runtime)) {
        return manual_review("provider_action_start_failed",
                             "provider action could not enter the running state");
    }

    // There is still no external effect here, so a cancellation observed before
    // the marker can be acknowledged normally and the provider is never invoked.
    if (context.cancellation_requested()) {
        if (!action_runtime_.transition(action.id, ActionStatus::failed_permanent)) {
            return manual_review("provider_pre_call_cancel_state_conflict",
                                 "provider call was cancelled before invocation but the action could not be closed");
        }
        return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
    }

    // This durable write MUST happen before invoke(). From this point onward a
    // crash, exception, timeout, or ambiguous transport result is never replayed.
    if (!action_runtime_.record_effect_started(action.id)) {
        return manual_review("provider_effect_marker_failed",
                             "provider effect uncertainty could not be persisted before invocation");
    }

    ProviderResult provider_result;
    try {
        provider_result = provider_.invoke(ProviderRequest{
            key,
            context.task.input_content_type,
            context.task.input,
            context.task.limits.max_output_bytes,
            context.task.limits.max_runtime});
    } catch (const std::exception& error) {
        static_cast<void>(action_runtime_.record_unknown_result(action.id));
        return manual_review("provider_exception", error.what());
    } catch (...) {
        static_cast<void>(action_runtime_.record_unknown_result(action.id));
        return manual_review("provider_exception",
                             "provider threw a non-standard exception");
    }

    switch (provider_result.outcome) {
    case ProviderOutcome::succeeded:
        if (!action_runtime_.record_confirmed_result(action.id)) {
            return manual_review("provider_confirmation_persistence_failed",
                                 "provider succeeded but confirmation could not be persisted");
        }
        if (provider_result.content_type.empty()) {
            return manual_review("invalid_provider_result",
                                 "successful provider result has no content type");
        }
        return HandlerResult{HandlerOutcome::succeeded,
                             std::move(provider_result.content_type),
                             std::move(provider_result.output), {}, {}};

    case ProviderOutcome::rejected:
        if (!action_runtime_.record_confirmed_result(action.id)) {
            return manual_review("provider_confirmation_persistence_failed",
                                 "provider rejection was definite but confirmation could not be persisted");
        }
        if (provider_result.failure_code.empty()) {
            provider_result.failure_code = "provider_rejected";
        }
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             std::move(provider_result.failure_code),
                             std::move(provider_result.failure_message)};

    case ProviderOutcome::effect_unknown:
        if (!action_runtime_.record_unknown_result(action.id)) {
            return manual_review("provider_unknown_persistence_failed",
                                 "provider result is ambiguous and the action could not be moved to manual review");
        }
        return manual_review(
            provider_result.failure_code.empty()
                ? "provider_effect_unknown"
                : std::move(provider_result.failure_code),
            provider_result.failure_message.empty()
                ? "provider result is ambiguous; automatic replay is forbidden"
                : std::move(provider_result.failure_message));
    }

    return manual_review("invalid_provider_result", "unknown provider outcome");
}

} // namespace gaudere_agent
