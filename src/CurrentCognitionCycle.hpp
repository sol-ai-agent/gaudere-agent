#ifndef GAUDERE_AGENT_CURRENT_COGNITION_CYCLE_HPP
#define GAUDERE_AGENT_CURRENT_COGNITION_CYCLE_HPP

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* current_cognition_task_kind =
    "cognition.current.v0";
inline constexpr const char* current_cognition_task_prefix =
    "cognition.current.v0:";
inline constexpr std::chrono::minutes current_cognition_max_snapshot_age{15};

/** Strict read-only validation for one durable cognition.current.v0 Task. */
[[nodiscard]] bool valid_current_cognition_task(
    const gaudere::work::Task& task) noexcept;

/**
 * Return the frozen current-context capture time only for a canonical Task.
 * Used by later provider gates to re-check freshness immediately before effects.
 */
[[nodiscard]] std::optional<std::int64_t>
current_cognition_snapshot_captured_at_ms(
    const gaudere::work::Task& task) noexcept;

enum class CurrentCognitionClaimResult {
    accepted,
    duplicate,
    disabled,
    predecessor_not_found,
    snapshot_not_found,
    ineligible,
    stale,
    conflict,
    unavailable
};

struct CurrentCognitionClaim {
    CurrentCognitionClaimResult result = CurrentCognitionClaimResult::ineligible;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free creator for one repeatable current-cognition Task.
 *
 * Identity is derived from one canonical succeeded predecessor cognition decision
 * plus one immutable current-context snapshot. The resulting Task already stores
 * the exact bounded text/plain provider contract, but this class owns no provider,
 * Action, budget, secret, network, WakeIntent or successor authority.
 */
class CurrentCognitionCycle {
public:
    using Now = std::function<gaudere::work::TimePoint()>;

    CurrentCognitionCycle(gaudere::work::TaskStore& task_store,
                          gaudere::work::Runtime& work_runtime,
                          Now now,
                          bool enabled = false);

    [[nodiscard]] CurrentCognitionClaim claim(
        const std::string& predecessor_task_id,
        const std::string& snapshot_task_id);

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    bool enabled_ = false;
};

} // namespace gaudere_agent

#endif
