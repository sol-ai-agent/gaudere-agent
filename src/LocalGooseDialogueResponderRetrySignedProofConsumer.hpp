#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_SIGNED_PROOF_CONSUMER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_SIGNED_PROOF_CONSUMER_HPP

#include "LocalGooseDialogueResponderRetryProofReservationInspector.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace gaudere_agent {

// This deliberately non-authoritative isolated function consumes ONE
// synthetic or externally supplied proof against a matching reservation.
// It never creates an executable grant, lease, decision, intent or Task.
// An attacker choosing their own pinned key can burn their own challenge;
// trustworthy operator enrollment is NOT implemented here.
enum class LocalGooseDialogueResponderRetrySignedProofConsumeCode : std::uint8_t {
    burned_no_authority,
    invalid_signed_message_or_expired,
    invalid_expected_reservation,
    already_burned_uncertain_or_mismatch,
    ledger_unavailable_or_commit_uncertain
};

// Evidence digest is a canonical SHA-256 over exact signed bytes and
// fingerprints. A successful burn is NOT consent or an execution token.
[[nodiscard]] LocalGooseDialogueResponderRetrySignedProofConsumeCode
consume_local_goose_dialogue_responder_retry_signed_proof_once(
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
