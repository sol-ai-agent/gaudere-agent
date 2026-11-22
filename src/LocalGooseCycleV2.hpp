#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_V2_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_V2_HPP

#include "LocalGooseCycleMemory.hpp"

#include <gaudere/work/Task.hpp>
#include <cstdint>
#include <string>

namespace gaudere_agent {

// SOURCE-ONLY. This is not an activation, cursor migration or runtime handler.
inline constexpr const char* local_goose_cycle_v2_schema =
    "gaudere.cognition.local-goose-cycle.v2";
inline constexpr const char* local_goose_cycle_v2_task_kind =
    "cognition.local-goose-cycle.v2";
inline constexpr const char* local_goose_cycle_v2_task_prefix =
    "cognition.local-goose-cycle.v2:";
inline constexpr const char* local_goose_cycle_v2_content_type =
    "application/vnd.gaudere.local-goose-cycle-v2+json";

struct LocalGooseCycleV2Inspection {
    bool eligible = false;
    std::string detail;
    std::string canonical_input;
    std::string memory_json;
    std::uint64_t generation = 0;
    std::string predecessor_task_id;
};

// Pure prototype for a separately gated, *hypothetical* first V2 opportunity.
// The predecessor must be a canonical V1 success. The bounded reader is used
// only by the source-only memory builder (at most three exact-ID lookups).
// The caller, NOT this function, must independently establish cursor ownership,
// generation, fixed model, anchor and the authority for any V1->V2 transition.
// No Task is submitted, stored, scheduled or run.
[[nodiscard]] gaudere::work::Task make_local_goose_cycle_v2_task(
    const gaudere::work::Task& anchor_observation,
    const std::string& model_sha256,
    std::uint64_t generation,
    std::int64_t due_at_ms,
    std::int64_t captured_at_ms,
    const gaudere::work::Task& predecessor_v1,
    const LocalGooseCycleMemoryRead& read_predecessor);

[[nodiscard]] LocalGooseCycleV2Inspection inspect_local_goose_cycle_v2_task(
    const gaudere::work::Task& task) noexcept;

// Prompt rendering is pure. Historical text is untrusted data and never
// grants permissions. The entire prompt has a 48-KiB hard cap.
[[nodiscard]] std::string local_goose_cycle_v2_prompt(
    const LocalGooseCycleV2Inspection& inspected);

} // namespace gaudere_agent
#endif
