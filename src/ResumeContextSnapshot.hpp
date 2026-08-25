#ifndef GAUDERE_AGENT_RESUME_CONTEXT_SNAPSHOT_HPP
#define GAUDERE_AGENT_RESUME_CONTEXT_SNAPSHOT_HPP

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* resume_context_snapshot_schema =
    "gaudere.resume-current-context.v1";
inline constexpr const char* resume_context_snapshot_task_kind =
    "continuity.resume-context-snapshot.v1";
inline constexpr const char* resume_context_snapshot_task_prefix =
    "continuity.resume-context-snapshot.v1:";
inline constexpr const char* resume_context_snapshot_content_type =
    "application/vnd.gaudere.resume-current-context+json";

enum class ResumeContextSnapshotRecordResult {
    accepted,
    duplicate,
    invalid,
    conflict,
    unavailable
};

struct ResumeContextSnapshotRecord {
    ResumeContextSnapshotRecordResult result =
        ResumeContextSnapshotRecordResult::invalid;
    std::optional<gaudere::work::Task> task;
    std::string detail;
};

/** Strict read-only interpretation of a caller-supplied snapshot request. */
struct ResumeContextSnapshotRequestInspection {
    bool eligible = false;
    std::string canonical_request;
    std::string detail;
};

/**
 * Validate and canonicalize one caller-supplied snapshot request without mutating
 * durable state. This is the same schema/bounds validation used by the recorder.
 */
[[nodiscard]] ResumeContextSnapshotRequestInspection
inspect_resume_context_snapshot_request(const std::string& request_json) noexcept;

/** Strict read-only interpretation of one already-durable snapshot Task. */
struct ResumeContextSnapshotInspection {
    bool eligible = false;
    std::int64_t captured_at_ms = 0;
    std::string canonical_capsule;
    std::string detail;
};

/**
 * Validate one terminal snapshot Task exactly as a future resume claim must see it.
 * No state is mutated and no provenance source is fetched implicitly.
 */
[[nodiscard]] ResumeContextSnapshotInspection inspect_resume_context_snapshot(
    const gaudere::work::Task& task) noexcept;

/** Provider-free durable recorder for bounded current-context capsules.
 *
 * The caller supplies JSON with schema/content_type/content/provenance but may not
 * choose captured_at_ms or the Task identity. The recorder inserts its own clock,
 * canonicalizes and validates the capsule, hashes the canonical bytes with SHA-256,
 * and persists a content-addressed Task. Successful local execution echoes exactly
 * those canonical bytes as the Task result.
 *
 * This component has no provider, secret, Action, WakeIntent, network or shell
 * dependency. The supplied work Runtime must already be in running state; recovery
 * remains an explicit caller responsibility.
 */
class ResumeContextSnapshotRecorder {
public:
    using Now = std::function<gaudere::work::TimePoint()>;

    ResumeContextSnapshotRecorder(gaudere::work::TaskStore& task_store,
                                  gaudere::work::Runtime& work_runtime,
                                  Now now);

    [[nodiscard]] ResumeContextSnapshotRecord record(
        const std::string& request_json);

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::work::Runtime& work_runtime_;
    Now now_;
};

} // namespace gaudere_agent

#endif
