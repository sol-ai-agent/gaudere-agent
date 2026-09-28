#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_sidecar_schema = 1;
inline constexpr std::uint32_t local_goose_dialogue_responder_message_dialogue = 1u << 0;
inline constexpr std::uint32_t local_goose_dialogue_responder_message_feedback = 1u << 1;
inline constexpr std::uint32_t local_goose_dialogue_responder_message_intervention = 1u << 2;
inline constexpr std::uint32_t local_goose_dialogue_responder_message_observation = 1u << 3;
inline constexpr std::uint32_t local_goose_dialogue_responder_message_all =
    local_goose_dialogue_responder_message_dialogue
    | local_goose_dialogue_responder_message_feedback
    | local_goose_dialogue_responder_message_intervention
    | local_goose_dialogue_responder_message_observation;

inline constexpr const char* local_goose_dialogue_responder_intent_prefix =
    "cognition.local-goose-dialogue.responder-intent.v1:";
inline constexpr const char* local_goose_dialogue_responder_request_prefix =
    "responder-v1-";

enum class LocalGooseDialogueResponderLeaseState : std::uint8_t {
    active = 0,
    exhausted = 1,
    expired = 2,
    revoked = 3,
    manual_review = 4
};

enum class LocalGooseDialogueResponderIntentState : std::uint8_t {
    prepared = 0,
    submitted = 1,
    completed = 2,
    conflict = 3,
    expired = 4,
    manual_review = 5
};

struct LocalGooseDialogueResponderLease {
    std::string lease_id;
    std::string thread_alias;
    std::string speaker_kind = "system";
    std::string speaker_id;
    std::uint32_t allowed_message_kinds = 0;
    std::string purpose;
    std::uint64_t max_system_turns = 0;
    std::uint64_t turns_committed = 0;
    std::int64_t issued_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::int64_t min_interval_ms = 0;
    std::int64_t next_eligible_at_ms = 0;
    LocalGooseDialogueResponderLeaseState state =
        LocalGooseDialogueResponderLeaseState::active;
    std::string terminal_reason;
    std::optional<std::int64_t> terminal_at_ms;
};

struct LocalGooseDialogueResponderIntent {
    std::string intent_id;
    std::string lease_id;
    std::string thread_alias;
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::string triggering_task_id;
    std::uint64_t expected_thread_revision = 0;
    std::string expected_head_task_id;
    std::string speaker_kind = "system";
    std::string speaker_id;
    std::string message_kind;
    std::string message;
    std::string request_id;
    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    LocalGooseDialogueResponderIntentState state =
        LocalGooseDialogueResponderIntentState::prepared;
    std::string submitted_task_id;
    std::optional<std::uint64_t> submitted_thread_revision;
    std::optional<std::int64_t> committed_at_ms;
    std::optional<std::int64_t> completed_at_ms;
    std::string terminal_reason;
};

[[nodiscard]] std::uint32_t local_goose_dialogue_responder_message_kind_bit(
    const std::string& message_kind) noexcept;

[[nodiscard]] bool valid_local_goose_dialogue_responder_lease(
    const LocalGooseDialogueResponderLease& lease) noexcept;

[[nodiscard]] bool valid_local_goose_dialogue_responder_intent(
    const LocalGooseDialogueResponderIntent& intent) noexcept;

[[nodiscard]] LocalGooseDialogueResponderIntent
make_local_goose_dialogue_responder_intent(
    const std::string& lease_id,
    const std::string& thread_alias,
    std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    const std::string& triggering_task_id,
    std::uint64_t expected_thread_revision,
    const std::string& expected_head_task_id,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message,
    std::int64_t created_at_ms,
    std::int64_t expires_at_ms);

} // namespace gaudere_agent

#endif
