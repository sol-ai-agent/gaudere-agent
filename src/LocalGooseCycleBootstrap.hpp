#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_BOOTSTRAP_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_BOOTSTRAP_HPP

#include "LocalActivityPulseStore.hpp"
#include "LocalGooseCycleStore.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseCycleBootstrapResult {
    seeded,
    duplicate,
    activated,
    already_active,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseCycleBootstrapStep {
    LocalGooseCycleBootstrapResult result =
        LocalGooseCycleBootstrapResult::invalid;
    std::optional<LocalGooseCycleCursor> cursor;
    std::string detail;
};

[[nodiscard]] LocalGooseCycleBootstrapStep seed_local_goose_cycle(
    LocalGooseCycleStore& cycle_store,
    gaudere::work::TaskStore& task_store,
    const LocalActivityPulseCursor& activity_cursor);

[[nodiscard]] LocalGooseCycleBootstrapStep activate_local_goose_cycle(
    LocalGooseCycleStore& cycle_store,
    std::int64_t due_at_ms);

} // namespace gaudere_agent

#endif
