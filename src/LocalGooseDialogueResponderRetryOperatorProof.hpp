#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_PROOF_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_PROOF_HPP

#include "LocalGooseDialogueResponderRetryGrantProposal.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// This v1 contract is an isolated, read-only cryptographic evidence check.
// It is NOT an approval service, a credential store or execution authority.
// The trusted pinned key MUST originate from a separately authorized
// operator-enrollment ceremony, NEVER from the candidate or signed payload.
inline constexpr std::int64_t local_goose_dialogue_responder_retry_proof_max_ttl_ms =
    5LL * 60 * 1000;

struct LocalGooseDialogueResponderRetryOperatorChallenge {
    std::string challenge_id;
    std::string proposal_id;
    std::string scope_id;
    std::string lineage_id;
    std::string approval_reference;
    std::string nonce_hex;
    std::int64_t issued_at_ms = -1;
    std::int64_t expires_at_ms = -1;
};

struct LocalGooseDialogueResponderRetrySignedOperatorProof {
    std::string challenge_id;
    std::string operator_id;
    std::string decision;
    std::string key_fingerprint;
    std::string signature_hex;
};

enum class LocalGooseDialogueResponderRetryOperatorProofCode : std::uint8_t {
    cryptographically_consistent_no_authority,
    invalid_proposal_or_scope,
    invalid_challenge,
    expired,
    untrusted_key_or_actor,
    signature_invalid,
    unavailable
};

struct LocalGooseDialogueResponderRetryOperatorProofStatus {
    LocalGooseDialogueResponderRetryOperatorProofCode code =
        LocalGooseDialogueResponderRetryOperatorProofCode::unavailable;
    std::string detail;
};

/**
 * Create an immutable challenge, with an externally supplied 256-bit nonce.
 * The caller must generate the nonce using a cryptographically secure,
 * trusted source and persist challenge consumption atomically elsewhere.
 * This function cannot establish entropy, unique issuance or human presence.
 */
[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryOperatorChallenge>
make_local_goose_dialogue_responder_retry_operator_challenge(
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const std::string& nonce_hex,
    std::int64_t issued_at_ms,
    std::int64_t ttl_ms);

[[nodiscard]] std::optional<std::string>
local_goose_dialogue_responder_retry_operator_signing_payload(
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const std::string& operator_id,
    const std::string& decision);

[[nodiscard]] LocalGooseDialogueResponderRetryOperatorProofStatus
inspect_local_goose_dialogue_responder_retry_operator_signature(
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedOperatorProof& signed_proof,
    const std::array<unsigned char, 32>& separately_pinned_ed25519_public_key,
    const std::string& separately_pinned_operator_id,
    std::int64_t now_ms);

} // namespace gaudere_agent

#endif
