#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_MEMORY_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_MEMORY_HPP

#include <gaudere/work/Task.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseCycleMemoryStatus {
    verified, unavailable, noncanonical, oversized
};

struct LocalGooseCycleMemory {
    LocalGooseCycleMemoryStatus status = LocalGooseCycleMemoryStatus::unavailable;
    // Only populated when every included Task and immediate link is verified.
    // Canonical JSON ordered oldest-to-newest; no openai_request text.
    std::string canonical_json;
    std::size_t included = 0;
    bool earlier_omitted = false;
};

// Read-only pure function. The caller must select the current canonical
// predecessor from a trusted, fixed-scope cursor on its owning thread.
// read_predecessor is an exact-ID bounded Task lookup, not a search.
// Never opens state.db, creates Tasks, runs a model or schedules a wake.
using LocalGooseCycleMemoryRead =
    std::function<std::optional<gaudere::work::Task>(const std::string&)>;

[[nodiscard]] LocalGooseCycleMemory build_local_goose_cycle_memory(
    const gaudere::work::Task& latest,
    const LocalGooseCycleMemoryRead& read_predecessor) noexcept;

} // namespace gaudere_agent
#endif
