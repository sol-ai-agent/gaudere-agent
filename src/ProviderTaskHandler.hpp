#ifndef GAUDERE_AGENT_PROVIDER_TASK_HANDLER_HPP
#define GAUDERE_AGENT_PROVIDER_TASK_HANDLER_HPP

#include "Provider.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <string>

namespace gaudere_agent {

/** Bridges one bounded task to one provider call through a recoverable Action.
 *
 * The Action is created before any provider effect. Immediately before invoke(),
 * record_effect_started() durably marks the effect as unknown. Any pre-existing
 * action from an earlier attempt is handled conservatively and never replayed.
 */
class ProviderTaskHandler final : public TaskHandler {
public:
    ProviderTaskHandler(gaudere::scheduling::wake::Runtime& action_runtime,
                        gaudere::scheduling::wake::ActionStore& action_store,
                        Provider& provider);

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    [[nodiscard]] std::string action_id(const gaudere::work::Task& task) const;
    [[nodiscard]] std::string action_key(const gaudere::work::Task& task) const;
    [[nodiscard]] HandlerResult existing_action_result(
        const gaudere::scheduling::wake::Action& action);
    [[nodiscard]] HandlerResult manual_review(std::string code,
                                              std::string message) const;

    gaudere::scheduling::wake::Runtime& action_runtime_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    Provider& provider_;
};

} // namespace gaudere_agent

#endif
