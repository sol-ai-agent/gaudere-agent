#ifndef GAUDERE_AGENT_LOCAL_CONTINUITY_OBSERVATION_HANDLER_HPP
#define GAUDERE_AGENT_LOCAL_CONTINUITY_OBSERVATION_HANDLER_HPP

#include "TaskExecutor.hpp"

namespace gaudere_agent {

/**
 * Pure local identity handler for canonical continuity observations.
 *
 * This handler owns no provider, network, Action, WakeIntent, secret, process or
 * host-control capability. It validates one exact canonical local-observation
 * Task and returns the canonical input bytes as the result.
 */
class LocalContinuityObservationHandler final : public TaskHandler {
public:
    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;
};

} // namespace gaudere_agent

#endif
