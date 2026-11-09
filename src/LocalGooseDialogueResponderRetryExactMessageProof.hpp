#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_EXACT_MESSAGE_PROOF_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_EXACT_MESSAGE_PROOF_HPP

#include "LocalGooseDialogueResponderRetryOperatorProof.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// Version 2 domain is intentionally INCOMPATIBLE with v1 proposal-only
// signatures. No v1 signed approval may authorize unspecified message bytes.
inline constexpr const char* local_goose_dialogue_responder_retry_exact_review_prefix =
    "cognition.local-goose-dialogue.responder-retry-exact-message-review.v1:";
inline constexpr std::size_t local_goose_dialogue_responder_retry_exact_max_message_bytes =
    4096;

// An inert, canonical representation of the EXACT proposed intervention.
// It can be shown to an operator by a FUTURE trusted UI but this code does
// not prove human display, enrollment, consent or durable anti-replay.
struct LocalGooseDialogueResponderRetryExactMessageReview {
    std::string review_id;
    std::string challenge_id;
    std::string proposal_id;
    std::string scope_id;
    std::string lineage_id;
    std::string message_sha256;
    std::string message_text;  // Exact UTF-8 bytes; not normalized or repaired
    std::string responder_speaker_kind = "system";
    std::string responder_speaker_id = "system-responder-v1";
    std::uint32_t retry_generation = 0;
    std::uint64_t completion_sequence = 0;
    std::uint64_t expected_head_revision = 0;
    std::uint32_t message_kind = 0;
};

struct LocalGooseDialogueResponderRetrySignedExactMessageProof {
    std::string review_id;
    std::string operator_id;
    std::string decision;
    std::string key_fingerprint;
    std::string signature_hex;
};

enum class LocalGooseDialogueResponderRetryExactMessageProofCode : std::uint8_t {
    cryptographically_consistent_no_authority,
    invalid_scope_or_proposal,
    invalid_challenge_or_message,
    expired,
    untrusted_key_or_actor,
    signature_invalid,
    unavailable
};

[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryExactMessageReview>
make_local_goose_dialogue_responder_retry_exact_message_review(
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const std::string& exact_utf8_intervention);

[[nodiscard]] bool valid_local_goose_dialogue_responder_retry_exact_message_review(
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge) noexcept;

// Caller may sign these bytes ONLY with a separately enrolled, operator-owned
// key after displaying the exact message and all consent fields. This function
// does not sign, request approval, generate a key or interact with a runtime.
[[nodiscard]] std::optional<std::string>
local_goose_dialogue_responder_retry_exact_message_signing_payload(
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const std::string& operator_id,
    const std::string& decision);

// Verifies an independently pinned Ed25519 key against the EXACT message,
// but never grants authority. Authentication of the pinned key, one-time
// challenge consumption, fresh eligibility and dispatch remain separate.
[[nodiscard]] LocalGooseDialogueResponderRetryExactMessageProofCode
inspect_local_goose_dialogue_responder_retry_exact_message_signature(
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& signed_proof,
    const std::array<unsigned char, 32>& separately_pinned_ed25519_public_key,
    const std::string& separately_pinned_operator_id,
    std::int64_t now_ms);

} // namespace gaudere_agent

#endif
