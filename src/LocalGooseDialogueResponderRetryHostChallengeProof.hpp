#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CHALLENGE_PROOF_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CHALLENGE_PROOF_HPP

#include "LocalGooseDialogueResponderRetryHostAnchorReceipt.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace gaudere_agent {

// A second, domain-separated host signature binds one exact #290 receipt to
// a caller-provided expected challenge and request context. This class does
// NOT generate the challenge, consume it, or authenticate its provenance.
inline constexpr const char* local_goose_dialogue_responder_retry_host_challenge_header =
    "gaudere-retry-host-challenge-proof.v2\n";
inline constexpr const char* local_goose_dialogue_responder_retry_host_challenge_domain =
    "gaudere-retry-host-challenge-proof-signature.v2|";

enum class LocalGooseDialogueResponderRetryHostChallengeCode : std::uint8_t {
    challenge_bound_cryptographic_consistency_no_authority,
    invalid_independent_trust_or_expectation,
    invalid_base_host_receipt,
    invalid_challenge_format,
    invalid_host_challenge_signature,
    wrong_exact_receipt_or_challenge_or_request,
    stale_or_outside_validity
};

// Inspect a signed short-lived v2 proof after independently validating the
// exact v1 host receipt. The host signer is expected to be a separate,
// genuinely enrolled identity, but this method CANNOT prove enrollment.
// All expected keys/fingerprints, challenge and request-context values, epoch
// floor and clock MUST be independently trusted by a future host controller.
// An attacker able to pick all inputs may self-attest.
// No nonce ledger, one-shot consume, auth, consent, grant, Task or ACK.
[[nodiscard]] LocalGooseDialogueResponderRetryHostChallengeCode
inspect_local_goose_dialogue_responder_retry_host_challenge_proof(
    const std::string& exact_v1_host_signed_receipt,
    const std::string& candidate_exact_v2_host_challenge_proof,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    std::uint64_t independently_trusted_minimum_anchor_epoch,
    const std::string& independently_expected_challenge_sha256,
    const std::string& independently_expected_request_context_sha256,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
