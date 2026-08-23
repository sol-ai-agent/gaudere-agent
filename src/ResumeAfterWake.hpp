#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_HPP

#include "ExplicitWake.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* resume_after_wake_task_kind =
    "cognition.resume-after-wake.v0";
inline constexpr const char* resume_after_wake_context_schema =
    "gaudere.cognition.resume-context.v1";
inline constexpr const char* resume_after_wake_decision_schema =
    "gaudere.cognition.resume-decision.v1";
inline constexpr const char* resume_after_wake_task_prefix =
    "cognition.resume-after-wake.v0:";

enum class ResumeAfterWakeClaimResult {
    accepted,
    duplicate,
    disabled,
    wake_not_found,
    ineligible,
    conflict,
    unavailable
};

enum class ResumeAfterWakeState {
    disabled,
    ineligible,
    eligible,
    claimed,
    completed,
    failed,
    manual_review
};

struct ResumeAfterWakeClaim {
    ResumeAfterWakeClaimResult result = ResumeAfterWakeClaimResult::ineligible;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

struct ResumeAfterWakeStatus {
    ResumeAfterWakeState state = ResumeAfterWakeState::ineligible;
    bool healthy = false;
    std::string report;
};

/** Provider-free authority boundary from one durable fired WakeIntent to one Task.
 *
 * This component cannot invoke a provider, create an Action, touch provider
 * budget, mutate WakeIntent, or schedule work by itself. Its sole mutation is an
 * idempotent gaudere::work::Runtime::submit() of one deterministic Task after
 * strict validation of the existing fixed-scope ExplicitWake lineage.
 *
 * The default is disabled. Initial v0 assumes the existing one-process,
 * one-database-mutator ownership model; multi-writer support requires a new
 * atomic claim design.
 */
class ResumeAfterWake {
public:
    ResumeAfterWake(gaudere::work::TaskStore& task_store,
                    ExplicitWake& explicit_wake,
                    gaudere::work::Runtime& work_runtime,
                    bool enabled = false) noexcept;

    [[nodiscard]] ResumeAfterWakeClaim claim(const std::string& wake_id);

    /** Read-only derived status. Never submits Tasks or mutates wake/provider state. */
    [[nodiscard]] ResumeAfterWakeStatus inspect(
        const std::string& wake_id) const;

private:
    gaudere::work::TaskStore& task_store_;
    ExplicitWake& explicit_wake_;
    gaudere::work::Runtime& work_runtime_;
    bool enabled_ = false;
};

} // namespace gaudere_agent

#endif
