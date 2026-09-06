#ifndef GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_HPP
#define GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_HPP

#include "LocalActivityPulseStore.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gaudere_agent {

enum class LocalActivityPulseResult {
    disabled,
    unseeded,
    seeded,
    duplicate,
    ineligible,
    not_due,
    preparing,
    waiting,
    settled,
    quiescent,
    clock_rollback,
    blocked,
    conflict,
    unavailable
};

struct LocalActivityPulseObservation {
    LocalActivityPulseResult result = LocalActivityPulseResult::unavailable;
    std::optional<LocalActivityPulseCursor> cursor;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free bounded local continuity activity.
 *
 * Authority is deliberately narrow: read exact existing Task/Action/Budget/Wake
 * evidence, mutate only the Agent-owned pulse sidecar, and create/execute the one
 * deterministic local-observation Task reserved by that cursor. There is no
 * provider, secret, network, WakeIntent mutation, Action mutation, cognition
 * creation, shell, B10 or host-control path in this class.
 */
class LocalActivityPulse {
public:
    using Now = std::function<gaudere::work::TimePoint()>;
    using PhaseHook = std::function<void(std::string_view)>;

    LocalActivityPulse(
        LocalActivityPulseStore& pulse_store,
        gaudere::work::TaskStore& task_store,
        gaudere::scheduling::wake::ActionStore& action_store,
        gaudere::budget::Store& budget_store,
        gaudere::scheduling::wake::WakeIntentStore& wake_store,
        gaudere::work::Runtime& work_runtime,
        Now now,
        bool enabled = false,
        PhaseHook phase_hook = {});

    /** Explicit one-shot seed from one exact succeeded continuity checkpoint. */
    [[nodiscard]] LocalActivityPulseObservation seed(
        const std::string& checkpoint_task_id);

    /** Advance/recover at most one admitted local observation generation. */
    [[nodiscard]] LocalActivityPulseObservation observe();

private:
    LocalActivityPulseStore& pulse_store_;
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::ActionStore& action_store_;
    gaudere::budget::Store& budget_store_;
    gaudere::scheduling::wake::WakeIntentStore& wake_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    bool enabled_ = false;
    PhaseHook phase_hook_;
};

} // namespace gaudere_agent

#endif
