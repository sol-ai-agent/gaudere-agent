#ifndef GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_PREPARE_HPP
#define GAUDERE_AGENT_RESUME_AFTER_WAKE_V1_PREPARE_HPP

#include "ResumeAfterWakeV1.hpp"
#include "ResumeContextSnapshot.hpp"

#include <gaudere/scheduling/wake/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace gaudere_agent {

inline constexpr const char* resume_after_wake_v1_selection_schema =
    "gaudere.resume-after-wake-v1-selection.v1";
inline constexpr const char* resume_after_wake_v1_selection_task_kind =
    "continuity.resume-after-wake-v1-selection.v1";
inline constexpr const char* resume_after_wake_v1_selection_task_prefix =
    "continuity.resume-after-wake-v1-selection.v1:";
inline constexpr const char* resume_after_wake_v1_selection_content_type =
    "application/vnd.gaudere.resume-after-wake-v1-selection+json";

struct ResumeAfterWakeV1Preparation {
    bool prepared = false;
    bool duplicate = false;
    std::optional<gaudere::work::Task> selection_task;
    std::optional<gaudere::work::Task> snapshot_task;
    ResumeAfterWakeV1Claim claim;
    std::string detail;
};

/**
 * Provider-free crash-safe preparation boundary for one fresh-context resume.
 *
 * The first durable selection Task freezes the exact caller request bytes and the
 * capture clock for one wake. A retry therefore reconstructs the same snapshot
 * identity even if the process crashed after selection or after snapshot commit.
 * The component records the snapshot and binds it through ResumeAfterWakeV1, but
 * never executes the resulting cognition Task and has no Provider/Action/secret/
 * network dependency.
 */
class ResumeAfterWakeV1Prepare {
public:
    using Now = std::function<gaudere::work::TimePoint()>;
    using Progress = std::function<void(std::string_view)>;

    ResumeAfterWakeV1Prepare(
        gaudere::work::TaskStore& task_store,
        gaudere::scheduling::wake::WakeIntentStore& wake_store,
        gaudere::work::Runtime& work_runtime,
        Now now,
        bool enabled = false,
        Progress progress = {});

    [[nodiscard]] ResumeAfterWakeV1Preparation prepare(
        const std::string& wake_id,
        const std::string& request_json);

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::WakeIntentStore& wake_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
    bool enabled_ = false;
    Progress progress_;
};

} // namespace gaudere_agent

#endif
