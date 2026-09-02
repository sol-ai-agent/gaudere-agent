#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_STALE_REFRESH_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_STALE_REFRESH_HPP

#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionPulseStore.hpp"

#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* autonomous_cognition_stale_retirement_reason =
    "autonomous pulse retired stale current cognition before provider effect";
inline constexpr const char* autonomous_cognition_provider_stale_detail =
    "pulse-prepared current context is stale at provider boundary";

enum class AutonomousCognitionStaleRefreshResult {
    not_applicable,
    retired,
    blocked,
    unavailable
};

struct AutonomousCognitionStaleRefreshObservation {
    AutonomousCognitionStaleRefreshResult result =
        AutonomousCognitionStaleRefreshResult::unavailable;
    std::optional<AutonomousCognitionPulseCursor> cursor;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free retirement of one stale, never-started pulse cognition.
 *
 * The provider gate remains the authority for canonical lineage, singleton and
 * freshness checks. This component may cancel only the exact pending Task named by
 * a canonical prepared pulse when the gate reports its exact stale-context block,
 * attempts_started is zero and no provider Action exists by id or idempotency key.
 * It then resets the same pulse generation to due/idle while preserving predecessor
 * evidence. The exact cancellation marker makes a crash after Task cancellation but
 * before cursor replacement recoverable without provider replay.
 */
class AutonomousCognitionStaleRefresh {
public:
    AutonomousCognitionStaleRefresh(
        AutonomousCognitionPulseStore& pulse_store,
        gaudere::work::TaskStore& task_store,
        gaudere::scheduling::wake::ActionStore& action_store,
        gaudere::work::Runtime& work_runtime,
        AutonomousCognitionProviderGate& provider_gate);

    [[nodiscard]] AutonomousCognitionStaleRefreshObservation step();

private:
    [[nodiscard]] AutonomousCognitionStaleRefreshObservation reset_retired(
        const AutonomousCognitionPulseCursor& cursor,
        const gaudere::work::Task& task);
    [[nodiscard]] bool provider_action_absent(
        const gaudere::work::Task& task) const;

    AutonomousCognitionPulseStore& pulse_store_;
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    gaudere::work::Runtime& work_runtime_;
    AutonomousCognitionProviderGate& provider_gate_;
};

} // namespace gaudere_agent

#endif
