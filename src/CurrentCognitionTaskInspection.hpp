#ifndef GAUDERE_AGENT_CURRENT_COGNITION_TASK_INSPECTION_HPP
#define GAUDERE_AGENT_CURRENT_COGNITION_TASK_INSPECTION_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {

/**
 * Read-only lineage extracted only after the authoritative
 * valid_current_cognition_task() validator has accepted the durable Task.
 */
struct CurrentCognitionTaskInspection {
    bool eligible = false;
    std::string predecessor_task_id;
    std::string predecessor_decision;
    std::string snapshot_task_id;
    std::string snapshot_capsule;
    std::int64_t captured_at_ms = -1;
    std::string detail;
};

[[nodiscard]] CurrentCognitionTaskInspection inspect_current_cognition_task(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
