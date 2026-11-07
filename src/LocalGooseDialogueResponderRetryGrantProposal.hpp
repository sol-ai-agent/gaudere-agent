#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_GRANT_PROPOSAL_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_GRANT_PROPOSAL_HPP

#include "LocalGooseDialogueResponderRetryScopeStore.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_responder_retry_grant_proposal_prefix =
    "cognition.local-goose-dialogue.responder-retry-grant-proposal.v1:";
inline constexpr const char* local_goose_dialogue_responder_retry_operator_source =
    "local-typed-operator-retry-v1";
inline constexpr std::int64_t local_goose_dialogue_responder_retry_max_grant_ttl_ms =
    15LL * 60 * 1000;
inline constexpr std::int64_t local_goose_dialogue_responder_retry_max_lease_ttl_ms =
    2LL * 60 * 1000;

enum class LocalGooseDialogueResponderRetryGrantProposalState : std::uint8_t {
    prepared_only = 0
};

/**
 * An inert, canonical PREPARATION ENVELOPE bound to one admitted retry scope.
 *
 * This is NOT a grant, a trusted approval or a lease. The approval_reference
 * is only a caller-supplied identifier; it has not been authenticated here.
 * Only a separate, future, explicit human-gated authority verifier may
 * authenticate the reference and then decide whether to activate anything.
 *
 * This type has no activation API, filesystem I/O, runtime or dispatch.
 */
struct LocalGooseDialogueResponderRetryGrantProposal {
    std::string proposal_id;
    std::string request_id;
    std::string authority_source = local_goose_dialogue_responder_retry_operator_source;
    std::string approval_reference;
    std::string scope_id;
    std::string lineage_id;
    std::string consumer_id;
    std::string event_id;
    std::string result_sha256;
    std::string task_id;
    std::string thread_alias;
    std::string observed_speaker_kind;
    std::string observed_speaker_id;
    std::string responder_speaker_kind = "system";
    std::string responder_speaker_id = "system-responder-v1";
    std::string purpose;
    std::uint32_t retry_generation = 0;
    std::uint64_t completion_sequence = 0;
    std::uint64_t expected_head_revision = 0;
    std::uint32_t allowed_message_kinds = 0;
    std::uint64_t max_system_turns = 0;
    std::int64_t max_lease_ttl_ms = 0;
    std::int64_t prepared_at_ms = -1;
    std::int64_t expires_at_ms = -1;
    LocalGooseDialogueResponderRetryGrantProposalState state =
        LocalGooseDialogueResponderRetryGrantProposalState::prepared_only;
};

[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryGrantProposal>
make_local_goose_dialogue_responder_retry_grant_proposal(
    const LocalGooseDialogueResponderRetryScope& scope,
    const std::string& request_id,
    const std::string& approval_reference,
    const std::string& purpose,
    std::int64_t prepared_at_ms,
    std::int64_t grant_ttl_ms,
    std::int64_t max_lease_ttl_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_retry_grant_proposal(
    const LocalGooseDialogueResponderRetryGrantProposal& proposal) noexcept;

[[nodiscard]] bool matches_local_goose_dialogue_responder_retry_grant_scope(
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope) noexcept;

} // namespace gaudere_agent

#endif
