#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_HPP

#include <gaudere/work/Task.hpp>

#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_schema =
    "gaudere.cognition.local-goose-dialogue.v1";
inline constexpr const char* local_goose_dialogue_response_schema =
    "gaudere.cognition.local-goose-dialogue-response.v1";
inline constexpr const char* local_goose_dialogue_task_kind =
    "cognition.local-goose-dialogue.v1";
inline constexpr const char* local_goose_dialogue_task_prefix =
    "cognition.local-goose-dialogue.v1:";
inline constexpr const char* local_goose_dialogue_content_type =
    "application/vnd.gaudere.local-goose-dialogue+json";
inline constexpr const char* local_goose_dialogue_response_content_type =
    "application/vnd.gaudere.local-goose-dialogue-response+json";

struct LocalGooseDialogueInspection {
    bool eligible = false;
    std::string detail;
    std::string request_id;
    std::string message;
    std::string message_sha256;
    std::string model_sha256;
    std::string canonical_input;
};

struct LocalGooseDialogueResponseInspection {
    bool eligible = false;
    std::string detail;
    std::string request_id;
    std::string response;
    std::string model_sha256;
    std::string canonical_response;
};

[[nodiscard]] gaudere::work::Task make_local_goose_dialogue_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256);

[[nodiscard]] LocalGooseDialogueInspection inspect_local_goose_dialogue_task(
    const gaudere::work::Task& task) noexcept;

[[nodiscard]] std::string make_local_goose_dialogue_response(
    const LocalGooseDialogueInspection& dialogue,
    const std::string& response);

[[nodiscard]] LocalGooseDialogueResponseInspection
inspect_local_goose_dialogue_response(
    const gaudere::work::Task& task,
    const std::string& raw) noexcept;

[[nodiscard]] bool canonical_local_goose_dialogue_success(
    const gaudere::work::Task& task) noexcept;

} // namespace gaudere_agent

#endif
