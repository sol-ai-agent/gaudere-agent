#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_HPP

#include "AutonomousCognitionPulseStore.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr std::chrono::hours autonomous_cognition_continue_cadence{6};
inline constexpr std::chrono::hours autonomous_cognition_quiescent_cadence{24};

enum class AutonomousCognitionPulseResult {
    disabled,
    unseeded,
    seeded,
    duplicate,
    not_due,
    budget_blocked,
    preparing,
    prepared,
    waiting,
    settled_continue,
    settled_stop,
    clock_rollback,
    blocked,
    conflict,
    unavailable
};

struct AutonomousCognitionPulseObservation {
    AutonomousCognitionPulseResult result =
        AutonomousCognitionPulseResult::unavailable;
    std::optional<AutonomousCognitionPulseCursor> cursor;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free recurring cognition preparation authority.
 *
 * This component may read Gaudere Tasks, durable budget eligibility and bounded
 * historical WakeIntent state; it may mutate only its Agent-owned pulse cursor and
 * create/recover local snapshot/current-cognition Tasks through existing provider-free
 * components. It owns no Provider, Action runtime, secret, network, shell or B10 path.
 */
class AutonomousCognitionPulse {
public:
    using Now = std::function<gaudere::work::TimePoint()>;

    AutonomousCognitionPulse(
        AutonomousCognitionPulseStore& pulse_store,
        gaudere::work::TaskStore& task_store,
        gaudere::budget::Store& budget_store,
        gaudere::scheduling::wake::WakeIntentStore& wake_store,
        gaudere::work::Runtime& work_runtime,
        Now now,
        bool enabled = false);

    /** Seed the fixed scope exactly once from one canonical succeeded current cognition. */
    [[nodiscard]] AutonomousCognitionPulseObservation seed(
        const std::string& predecessor_task_id);

    /** Observe/advance at most one fixed-scope generation without provider effects. */
    [[nodiscard]] AutonomousCognitionPulseObservation observe();

private:
    AutonomousCognitionPulseStore& pulse_store_;
    gaudere::work::TaskStore& task_store_;
    gaudere::budget::Store& budget_store_;
    gaudere::scheduling::wake::WakeIntentStore& wake_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    bool enabled_ = false;
};

} // namespace gaudere_agent

#endif
