#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_PROOF_RESERVATION_INSPECTOR_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_PROOF_RESERVATION_INSPECTOR_HPP

#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"
#include "LocalGooseDialogueResponderRetryChallengeLedger.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace gaudere_agent {

// These results are observations only. Even a consistent, reserved challenge
// is NOT an operator approval, anti-replay consumption, grant or permission
// to execute. This inspector never issues, burns or mutates a reservation.
enum class LocalGooseDialogueResponderRetryProofReservationCode : std::uint8_t {
    consistent_reserved_no_authority,
    invalid_exact_message_signature,
    invalid_expected_reservation,
    ledger_unavailable_or_unsafe,
    reservation_missing,
    reservation_mismatch,
    reservation_already_used_or_uncertain
};

// SHA-256 of a domain-separated, lowercase-hex nonce string. This is a
// *fingerprint*, NOT evidence that the nonce was securely generated.
[[nodiscard]] std::string
local_goose_dialogue_responder_retry_nonce_fingerprint(
    const std::string& nonce_hex);

// Opens ONLY an existing, owner-0600, read-only, schema-v1 ledger. The exact
// signed message, scope, challenge, nonce fingerprint, request, issued/expiry
// and state must match one immutable pending reservation. No transaction
// spanning this read and any future action exists: the result is NOT authority.
[[nodiscard]] LocalGooseDialogueResponderRetryProofReservationCode
inspect_local_goose_dialogue_responder_retry_proof_reservation(
    const std::string& existing_ledger_path,
    const std::string& expected_request_id,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& signed_proof,
    const std::array<unsigned char, 32>& separately_pinned_public_key,
    const std::string& separately_pinned_operator_id,
    std::int64_t now_ms);

} // namespace gaudere_agent

#endif
