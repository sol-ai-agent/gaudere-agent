#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_HPP

#include <gaudere/scheduling/wake/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* resume_after_wake_v1_task_kind =
    "cognition.resume-after-wake.v1";
inline constexpr const char* resume_after_wake_v1_task_prefix =
    "cognition.resume-after-wake.v1:";
inline constexpr const char* resume_after_wake_v1_context_schema =
    "gaudere.cognition.resume-context.v2";
inline constexpr const char* resume_after_wake_v1_content_type =
    "application/vnd.gaudere.cognition.resume-context+json";
inline constexpr std::chrono::minutes resume_after_wake_v1_max_snapshot_age{15};

enum class ResumeAfterWakeV1ClaimResult {
    accepted,
    duplicate,
    disabled,
    wake_not_found,
    snapshot_not_found,
    ineligible,
    stale,
    conflict,
    unavailable
};

struct ResumeAfterWakeV1Claim {
    ResumeAfterWakeV1ClaimResult result = ResumeAfterWakeV1ClaimResult::ineligible;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/**
 * Provider-free first-write binding between one fired WakeIntent and one fresh
 * immutable current-context snapshot.
 *
 * This class can only submit a deterministic `cognition.resume-after-wake.v1`
 * Task. It never constructs a provider, Action, secret, network client or wake.
 * The first accepted claim freezes one snapshot into the Task definition. On
 * reopen an existing canonical Task is validated before freshness is considered;
 * a different requested snapshot conflicts rather than creating a second identity.
 */
class ResumeAfterWakeV1 {
public:
    using Now = std::function<gaudere::work::TimePoint()>;

    ResumeAfterWakeV1(gaudere::work::TaskStore& task_store,
                      gaudere::scheduling::wake::WakeIntentStore& wake_store,
                      gaudere::work::Runtime& work_runtime,
                      Now now,
                      bool enabled = false);

    [[nodiscard]] ResumeAfterWakeV1Claim claim(
        const std::string& wake_id,
        const std::string& snapshot_id);

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::WakeIntentStore& wake_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    bool enabled_ = false;
};

} // namespace gaudere_agent

#endif
