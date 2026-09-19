#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_SERVICE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_SERVICE_HPP

#include "LocalGooseCycleStimulusStore.hpp"
#include "LocalGooseCycleStore.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseCycleStimulusServiceResult {
    accepted,
    duplicate,
    consumed,
    superseded,
    manual_review,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseCycleStimulusServiceStep {
    LocalGooseCycleStimulusServiceResult result =
        LocalGooseCycleStimulusServiceResult::invalid;
    std::optional<LocalGooseCycleStimulus> stimulus;
    std::optional<LocalGooseCycleCursor> cursor;
    std::string detail;
};

/**
 * Provider-free owner-side boundary for one explicit local recheck.
 *
 * This service may append one bounded durable stimulus and may reconcile that
 * stimulus into the already-defined Local Goose cycle dormant->scheduled CAS.
 * It never creates or executes a Task, calls Goose/OpenAI, owns a scheduler,
 * uses live control, or performs host effects.
 */
class LocalGooseCycleStimulusService {
public:
    LocalGooseCycleStimulusService(
        LocalGooseCycleStimulusStore& stimulus_store,
        LocalGooseCycleStore& cycle_store,
        gaudere::work::TaskStore& task_store);

    [[nodiscard]] LocalGooseCycleStimulusServiceStep accept_explicit_recheck(
        const std::string& source_id,
        std::int64_t accepted_at_ms);

    [[nodiscard]] LocalGooseCycleStimulusServiceStep reconcile(
        const std::string& stimulus_id,
        std::int64_t observed_at_ms);

private:
    LocalGooseCycleStimulusStore& stimulus_store_;
    LocalGooseCycleStore& cycle_store_;
    gaudere::work::TaskStore& task_store_;
};

} // namespace gaudere_agent

#endif
