#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_KEY_PIN_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_KEY_PIN_HPP

#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"

#include <cstdint>
#include <string>

namespace gaudere_agent {

// A LOCAL, inert read-only operator key pin. The trust anchor must be chosen
// OUTSIDE the request/Drive and OUTSIDE this API by the actual operator.
// A matching key is NOT evidence that a human saw or approved the message.
inline constexpr const char* local_goose_dialogue_responder_retry_key_pin_header =
    "gaudere-retry-operator-ed25519-key-pin.v1\n"
    "operator_id=bertrand\n"
    "public_key_hex=";

enum class LocalGooseDialogueResponderRetryPinnedSignatureCode : std::uint8_t {
    cryptographically_consistent_no_authority,
    key_pin_unavailable_or_unsafe,
    trusted_fingerprint_invalid_or_mismatch,
    signed_message_invalid_or_expired
};

// Only existing regular owner-0400 files with exactly one hard link, no
// symlink, canonical content and an independently pinned fingerprint are
// accepted. Never derives the expected fingerprint from this file or Drive.
// Does NOT generate a key, enroll a key, sign, burn nonce or dispatch.
[[nodiscard]] LocalGooseDialogueResponderRetryPinnedSignatureCode
inspect_local_goose_dialogue_responder_retry_pinned_signature(
    const std::string& trusted_absolute_pin_path,
    const std::string& independently_trusted_key_fingerprint,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& signed_proof,
    std::int64_t now_ms);

} // namespace gaudere_agent

#endif
