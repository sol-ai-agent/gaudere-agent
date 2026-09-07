#ifndef GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_cognition_schema =
    "gaudere.cognition.local-goose.v1";
inline constexpr const char* local_goose_cognition_task_kind =
    "cognition.local-goose.v1";
inline constexpr const char* local_goose_cognition_task_prefix =
    "cognition.local-goose.v1:";
inline constexpr const char* local_goose_cognition_content_type =
    "application/vnd.gaudere.local-goose-cognition+json";
inline constexpr const char* local_goose_decision_schema =
    "gaudere.cognition.local-goose.decision.v1";
inline constexpr const char* local_goose_decision_content_type =
    "application/vnd.gaudere.local-goose-decision+json";

struct LocalGooseCognitionInspection {
    bool eligible = false;
    std::string detail;
    std::string source_observation_task_id;
    std::string source_observation_result_sha256;
    std::string source_observation_payload;
    std::string model_sha256;
};

struct LocalGooseDecision {
    std::string decision;
    std::string assessment;
    std::string reason;
    std::optional<std::string> openai_request;
    std::optional<std::int64_t> next_wake_after_ms;
    std::string canonical_json;
};

struct LocalGooseDecisionInspection {
    bool eligible = false;
    std::string detail;
    LocalGooseDecision decision;
};

[[nodiscard]] gaudere::work::Task make_local_goose_cognition_task(
    const gaudere::work::Task& source_observation,
    const std::string& model_sha256);

[[nodiscard]] LocalGooseCognitionInspection inspect_local_goose_cognition_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] LocalGooseDecisionInspection inspect_local_goose_decision(
    const std::string& raw) noexcept;

[[nodiscard]] std::string local_goose_prompt(
    const LocalGooseCognitionInspection& cognition);

[[nodiscard]] bool canonical_local_goose_cognition_success(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
