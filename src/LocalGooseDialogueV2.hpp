#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V2_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V2_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_v2_schema =
    "gaudere.cognition.local-goose-dialogue.v2";
inline constexpr const char* local_goose_dialogue_v2_response_schema =
    "gaudere.cognition.local-goose-dialogue-v2-response.v1";
inline constexpr const char* local_goose_dialogue_v2_task_kind =
    "cognition.local-goose-dialogue.v2";
inline constexpr const char* local_goose_dialogue_v2_task_prefix =
    "cognition.local-goose-dialogue.v2:";
inline constexpr const char* local_goose_dialogue_v2_content_type =
    "application/vnd.gaudere.local-goose-dialogue-v2+json";
inline constexpr const char* local_goose_dialogue_v2_response_content_type =
    "application/vnd.gaudere.local-goose-dialogue-v2-response+json";

struct LocalGooseDialogueV2Inspection {
    bool eligible = false;
    std::string detail;
    std::string task_id;
    std::string request_id;
    std::string message;
    std::string message_sha256;
    std::string model_sha256;
    std::uint64_t turn_index = 0;
    std::string root_task_id;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    std::string canonical_input;
};

struct LocalGooseDialogueV2ResponseInspection {
    bool eligible = false;
    std::string detail;
    std::string request_id;
    std::string root_task_id;
    std::uint64_t turn_index = 0;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    std::string response;
    std::string model_sha256;
    std::string canonical_response;
};

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_v2_root_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256);

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_v2_successor_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256,
    const gaudere::work::Task& predecessor);

[[nodiscard]] LocalGooseDialogueV2Inspection
inspect_local_goose_dialogue_v2_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] std::string make_local_goose_dialogue_v2_response(
    const LocalGooseDialogueV2Inspection& dialogue,
    const std::string& response);

[[nodiscard]] LocalGooseDialogueV2ResponseInspection
inspect_local_goose_dialogue_v2_response(
    const gaudere::work::Task& task,
    const std::string& raw) noexcept;

[[nodiscard]] bool canonical_local_goose_dialogue_v2_success(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
