#ifndef GAUDERE_AGENT_PROVIDER_TASK_HANDLER_HPP
#define GAUDERE_AGENT_PROVIDER_TASK_HANDLER_HPP

#include "Provider.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <functional>
#include <string>

namespace gaudere_agent {

/** Bridges one bounded task to one provider call through a recoverable Action.
 *
 * A durable budget permit is consumed before a new provider Action is created.
 * The Action is then created before any provider effect. Immediately before invoke(),
 * record_effect_started() durably marks the effect as unknown. Any pre-existing
 * action from an earlier attempt is handled conservatively and never replayed.
 */
class ProviderTaskHandler final : public TaskHandler {
public:
    using BudgetNow = std::function<gaudere::budget::TimePoint()>;

    ProviderTaskHandler(gaudere::scheduling::wake::Runtime& action_runtime,
                        gaudere::scheduling::wake::ActionStore& action_store,
                        Provider& provider,
                        gaudere::budget::Store& budget_store,
                        gaudere::budget::Policy budget_policy,
                        BudgetNow budget_now);

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    [[nodiscard]] std::string action_id(const gaudere::work::Task& task) const;
    [[nodiscard]] std::string action_key(const gaudere::work::Task& task) const;
    [[nodiscard]] std::string budget_scope() const;
    [[nodiscard]] HandlerResult existing_action_result(
        const gaudere::scheduling::wake::Action& action);
    [[nodiscard]] HandlerResult manual_review(std::string code,
                                              std::string message) const;
    [[nodiscard]] HandlerResult budget_denied(
        gaudere::budget::ConsumeResult result) const;

    gaudere::scheduling::wake::Runtime& action_runtime_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    Provider& provider_;
    gaudere::budget::Store& budget_store_;
    gaudere::budget::Policy budget_policy_;
    BudgetNow budget_now_;
};

} // namespace gaudere_agent

#endif
