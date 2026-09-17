#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_HPP

#include "LocalGooseCognition.hpp"

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_cycle_schema =
    "gaudere.cognition.local-goose-cycle.v1";
inline constexpr const char* local_goose_cycle_task_kind =
    "cognition.local-goose-cycle.v1";
inline constexpr const char* local_goose_cycle_task_prefix =
    "cognition.local-goose-cycle.v1:";
inline constexpr const char* local_goose_cycle_content_type =
    "application/vnd.gaudere.local-goose-cycle+json";

struct LocalGooseCyclePredecessor {
    std::string task_id;
    std::string result_sha256;
    LocalGooseDecision decision;
};

struct LocalGooseCycleInspection {
    bool eligible = false;
    std::string detail;
    std::string anchor_observation_task_id;
    std::string anchor_observation_result_sha256;
    std::uint64_t generation = 0;
    std::int64_t due_at_ms = 0;
    std::int64_t captured_at_ms = 0;
    std::string model_sha256;
    std::optional<LocalGooseCyclePredecessor> predecessor;
    std::string canonical_input;
};

[[nodiscard]] gaudere::work::Task make_local_goose_cycle_task(
    const gaudere::work::Task& anchor_observation,
    const std::string& model_sha256,
    std::uint64_t generation,
    std::int64_t due_at_ms,
    std::int64_t captured_at_ms,
    const std::optional<gaudere::work::Task>& predecessor = std::nullopt);

[[nodiscard]] LocalGooseCycleInspection inspect_local_goose_cycle_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] bool canonical_local_goose_cycle_success(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] std::string local_goose_cycle_prompt(
    const LocalGooseCycleInspection& cycle);

} // namespace gaudere_agent

#endif
