#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_CONTROL_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_CONTROL_HPP

#include "LiveControl.hpp"
#include "LocalGooseCycleStimulusService.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace gaudere_agent {

/**
 * Bounded worker-side live-control adapter for explicit Local Goose rechecks.
 *
 * It accepts only the already-validated request identity supplied by
 * LiveControl. Durable acceptance/reconciliation remains owned by
 * LocalGooseCycleStimulusService. No Task, provider, shell, network or host
 * authority is introduced here.
 */
class LocalGooseCycleStimulusControl {
public:
    using NowMs = std::function<std::int64_t()>;

    LocalGooseCycleStimulusControl(
        LocalGooseCycleStimulusService& service,
        NowMs now_ms);

    [[nodiscard]] LiveControlReply stimulate(
        const std::string& request_id);

private:
    LocalGooseCycleStimulusService& service_;
    NowMs now_ms_;
};

} // namespace gaudere_agent

#endif
