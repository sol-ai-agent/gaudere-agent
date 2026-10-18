#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_HPP

#include "LocalGooseDialogueResponder.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_authority_sidecar_schema = 3;
inline constexpr const char* local_goose_dialogue_responder_grant_prefix =
    "cognition.local-goose-dialogue.responder-grant.v1:";
inline constexpr const char* local_goose_dialogue_responder_decision_prefix =
    "cognition.local-goose-dialogue.responder-decision.v1:";

enum class LocalGooseDialogueResponderGrantState : std::uint8_t {
    prepared = 0,
    activated = 1,
    revoked = 2,
    expired = 3,
    manual_review = 4
};

enum class LocalGooseDialogueResponderDecisionState : std::uint8_t {
    prepared = 0,
    intent_bound = 1,
    completed = 2,
    conflict = 3,
    expired = 4,
    manual_review = 5
};

struct LocalGooseDialogueResponderGrant {
    std::string grant_id;
    std::string request_id;
    std::string authority_source;
    std::string thread_alias;
    std::string speaker_kind = "system";
    std::string speaker_id;
    std::uint32_t allowed_message_kinds = 0;
    std::string purpose;
    std::uint64_t max_system_turns = 0;
    std::int64_t max_lease_ttl_ms = 0;
    std::int64_t min_interval_ms = 0;
    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    LocalGooseDialogueResponderGrantState state =
        LocalGooseDialogueResponderGrantState::prepared;
    std::optional<std::string> activated_lease_id;
    std::optional<std::int64_t> activated_at_ms;
    std::string terminal_reason;
    std::optional<std::int64_t> terminal_at_ms;
};

struct LocalGooseDialogueResponderDecision {
    std::string decision_id;
    std::string request_id;
    std::string authority_source;
    std::string grant_id;
    std::string lease_id;
    std::string consumer_id;
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::string thread_alias;
    std::uint64_t thread_revision = 0;
    std::string task_id;
    std::string observed_speaker_kind;
    std::string observed_speaker_id;
    std::uint64_t expected_head_revision = 0;
    std::string expected_head_task_id;
    std::string responder_speaker_kind = "system";
    std::string responder_speaker_id;
    std::string message_kind;
    std::string message;
    std::int64_t intent_ttl_ms = 0;
    std::int64_t created_at_ms = 0;
    LocalGooseDialogueResponderDecisionState state =
        LocalGooseDialogueResponderDecisionState::prepared;
    std::optional<std::string> intent_id;
    std::optional<std::int64_t> bound_at_ms;
    std::optional<std::int64_t> completed_at_ms;
    std::string terminal_reason;
    std::optional<std::int64_t> terminal_at_ms;
};

[[nodiscard]] LocalGooseDialogueResponderGrant
make_local_goose_dialogue_responder_grant(
    const std::string& request_id,
    const std::string& authority_source,
    const std::string& thread_alias,
    const std::string& speaker_id,
    std::uint32_t allowed_message_kinds,
    const std::string& purpose,
    std::uint64_t max_system_turns,
    std::int64_t max_lease_ttl_ms,
    std::int64_t min_interval_ms,
    std::int64_t created_at_ms,
    std::int64_t grant_ttl_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_grant(
    const LocalGooseDialogueResponderGrant& grant) noexcept;

[[nodiscard]] LocalGooseDialogueResponderDecision
make_local_goose_dialogue_responder_decision(
    const std::string& request_id,
    const std::string& authority_source,
    const std::string& grant_id,
    const std::string& lease_id,
    const std::string& consumer_id,
    std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    const std::string& thread_alias,
    std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& observed_speaker_kind,
    const std::string& observed_speaker_id,
    std::uint64_t expected_head_revision,
    const std::string& expected_head_task_id,
    const std::string& responder_speaker_id,
    const std::string& message_kind,
    const std::string& message,
    std::int64_t intent_ttl_ms,
    std::int64_t created_at_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_decision(
    const LocalGooseDialogueResponderDecision& decision) noexcept;

} // namespace gaudere_agent

#endif
