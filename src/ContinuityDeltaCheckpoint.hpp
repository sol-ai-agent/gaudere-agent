#ifndef GAUDERE_AGENT_CONTINUITY_DELTA_CHECKPOINT_HPP
#define GAUDERE_AGENT_CONTINUITY_DELTA_CHECKPOINT_HPP

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gaudere_agent {

inline constexpr const char* continuity_delta_checkpoint_schema =
    "gaudere.continuity.delta-checkpoint.v1";
inline constexpr const char* continuity_delta_checkpoint_task_kind =
    "continuity.delta-checkpoint.v1";
inline constexpr const char* continuity_delta_checkpoint_task_prefix =
    "continuity.delta-checkpoint.v1:";
inline constexpr const char* continuity_delta_checkpoint_content_type =
    "application/vnd.gaudere.continuity-delta-checkpoint+json";

enum class ContinuityDeltaCheckpointResult {
    accepted,
    duplicate,
    ineligible,
    conflict,
    unavailable
};

struct ContinuityDeltaCheckpointRecord {
    ContinuityDeltaCheckpointResult result =
        ContinuityDeltaCheckpointResult::ineligible;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free local continuity journal.
 *
 * The checkpoint reads only Gaudere-owned Task/Action/Budget/Wake durable state.
 * It verifies one succeeded autonomous current-cognition Task against its exact
 * predecessor and both expected confirmed provider Action markers, then persists
 * one deterministic local checkpoint Task/result. It has no provider, secret,
 * network, shell, WakeIntent mutation, successor or external-action authority.
 *
 * One checkpoint idempotency key is permanently bound to the audited cognition.
 * If later facts drift, a second semantic checkpoint is rejected as conflict
 * rather than rewriting or silently superseding the first durable record.
 */
class ContinuityDeltaCheckpoint {
public:
    using Now = std::function<gaudere::work::TimePoint()>;
    using PhaseHook = std::function<void(std::string_view)>;

    ContinuityDeltaCheckpoint(
        gaudere::work::TaskStore& task_store,
        gaudere::scheduling::wake::ActionStore& action_store,
        gaudere::budget::Store& budget_store,
        gaudere::scheduling::wake::WakeIntentStore& wake_store,
        gaudere::work::Runtime& work_runtime,
        Now now,
        PhaseHook phase_hook = {});

    [[nodiscard]] ContinuityDeltaCheckpointRecord checkpoint(
        const std::string& audited_task_id);

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    gaudere::budget::Store& budget_store_;
    gaudere::scheduling::wake::WakeIntentStore& wake_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    PhaseHook phase_hook_;
};

} // namespace gaudere_agent

#endif
