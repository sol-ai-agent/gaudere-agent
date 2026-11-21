#ifndef GAUDERE_AGENT_LOCAL_GOOSE_SETTLED_INSPECTION_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_SETTLED_INSPECTION_HPP

#include "LocalGooseCycleStore.hpp"
#include <gaudere/work/Task.hpp>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseSettledStatus {
    verified, unavailable, no_predecessor, not_applicable,
    snapshot_changed, noncanonical
};
enum class LocalGooseSettledDecision { idle, request_openai };

struct LocalGooseSettledEvidence {
    std::uint64_t cursor_revision;
    std::uint64_t cursor_generation;
    std::uint64_t predecessor_generation;
    LocalGooseSettledDecision decision;
    // No non-null scheduling intent is consistent with this dormant-only contract.
    std::optional<std::int64_t> next_wake_after_ms;
    std::string result_sha256;
    // Task status is necessarily succeeded for verified evidence.
};
struct LocalGooseSettledInspection {
    LocalGooseSettledStatus status = LocalGooseSettledStatus::unavailable;
    std::optional<LocalGooseSettledEvidence> evidence;
};

using LocalGooseCursorRead =
    std::function<std::optional<LocalGooseCycleCursor>()>;
using LocalGooseTaskRead =
    std::function<std::optional<gaudere::work::Task>(const std::string&)>;

// Source-only primitive. Call synchronously on the sole Agent owner thread.
// The trusted readers must be read-only, bounded, fixed-scope store lookups;
// the model digest comes from the trusted active configuration, not a caller.
// At most two cursor reads and three exact Task reads. No recursive audit.
// No Runtime, provider, scheduler, transport or write interface is exposed.
[[nodiscard]] LocalGooseSettledInspection inspect_local_goose_settled_decision(
    const LocalGooseCursorRead& read_cursor,
    const LocalGooseTaskRead& read_task,
    const std::string& active_model_sha256) noexcept;

} // namespace gaudere_agent
#endif
