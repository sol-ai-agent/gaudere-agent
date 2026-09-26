#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V3_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_V3_HPP

#include <gaudere/work/Task.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_v3_schema =
    "gaudere.cognition.local-goose-dialogue.v3";
inline constexpr const char* local_goose_dialogue_v3_response_schema =
    "gaudere.cognition.local-goose-dialogue-v3-response.v1";
inline constexpr const char* local_goose_dialogue_v3_task_kind =
    "cognition.local-goose-dialogue.v3";
inline constexpr const char* local_goose_dialogue_v3_task_prefix =
    "cognition.local-goose-dialogue.v3:";
inline constexpr const char* local_goose_dialogue_v3_content_type =
    "application/vnd.gaudere.local-goose-dialogue-v3+json";
inline constexpr const char* local_goose_dialogue_v3_response_content_type =
    "application/vnd.gaudere.local-goose-dialogue-v3-response+json";

inline constexpr const char* local_goose_dialogue_v3_speaker_human = "human";
inline constexpr const char* local_goose_dialogue_v3_speaker_system = "system";

inline constexpr const char* local_goose_dialogue_v3_message_dialogue = "dialogue";
inline constexpr const char* local_goose_dialogue_v3_message_feedback = "feedback";
inline constexpr const char* local_goose_dialogue_v3_message_intervention = "intervention";
inline constexpr const char* local_goose_dialogue_v3_message_observation = "observation";

struct LocalGooseDialogueV3Inspection {
    bool eligible = false;
    std::string detail;
    std::string task_id;
    std::string request_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string message;
    std::string message_sha256;
    std::string model_sha256;
    std::uint64_t turn_index = 0;
    std::string root_task_id;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    std::string canonical_input;
};

struct LocalGooseDialogueV3ResponseInspection {
    bool eligible = false;
    std::string detail;
    std::string request_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string root_task_id;
    std::uint64_t turn_index = 0;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    std::string response;
    std::string model_sha256;
    std::string canonical_response;
};

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_v3_root_task(
    const std::string& request_id,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message,
    const std::string& model_sha256);

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_v3_successor_task(
    const std::string& request_id,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message,
    const std::string& model_sha256,
    const gaudere::work::Task& predecessor);

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_v3_bridge_from_v2_task(
    const std::string& request_id,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message,
    const std::string& model_sha256,
    const gaudere::work::Task& predecessor_v2);

[[nodiscard]] LocalGooseDialogueV3Inspection
inspect_local_goose_dialogue_v3_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] std::string make_local_goose_dialogue_v3_response(
    const LocalGooseDialogueV3Inspection& dialogue,
    const std::string& response);

[[nodiscard]] LocalGooseDialogueV3ResponseInspection
inspect_local_goose_dialogue_v3_response(
    const gaudere::work::Task& task,
    const std::string& raw) noexcept;

[[nodiscard]] bool canonical_local_goose_dialogue_v3_success(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
