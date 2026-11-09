#include "LocalGooseDialogueResponderRetryProofReservationInspector.hpp"

#include "Sha256.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryProofReservationCode;
using Record = LocalGooseDialogueResponderRetryChallengeRecord;
using Reservation = LocalGooseDialogueResponderRetryChallengeReservation;
using Ledger = LocalGooseDialogueResponderRetryChallengeLedger;
using OpenMode = LocalGooseDialogueResponderRetryChallengeLedgerOpenMode;
using State = LocalGooseDialogueResponderRetryChallengeState;
using ExactCode = LocalGooseDialogueResponderRetryExactMessageProofCode;

constexpr const char* nonce_domain =
    "gaudere.cognition.local-goose-dialogue-responder-retry-nonce-fingerprint.v1|";

bool same_reservation(const Reservation& a, const Reservation& b) noexcept
{
    return a.reservation_id == b.reservation_id
        && a.request_id == b.request_id
        && a.challenge_id == b.challenge_id
        && a.proposal_id == b.proposal_id
        && a.scope_id == b.scope_id
        && a.lineage_id == b.lineage_id
        && a.nonce_sha256 == b.nonce_sha256
        && a.retry_generation == b.retry_generation
        && a.issued_at_ms == b.issued_at_ms
        && a.expires_at_ms == b.expires_at_ms;
}

} // namespace

std::string local_goose_dialogue_responder_retry_nonce_fingerprint(
    const std::string& nonce_hex)
{
    if (nonce_hex.size() != 64) return {};
    bool nonzero = false;
    for (const unsigned char c : nonce_hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return {};
        if (c != '0') nonzero = true;
    }
    if (!nonzero) return {};
    return sha256_hex(std::string(nonce_domain) + nonce_hex);
}

Code inspect_local_goose_dialogue_responder_retry_proof_reservation(
    const std::string& existing_ledger_path,
    const std::string& expected_request_id,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& signed_proof,
    const std::array<unsigned char, 32>& separately_pinned_public_key,
    const std::string& separately_pinned_operator_id,
    const std::int64_t now_ms)
{
    // Revalidate the ENTIRE exact-message proof, including the signed bytes,
    // historical scope and the challenge's clock window, on every inspection.
    if (inspect_local_goose_dialogue_responder_retry_exact_message_signature(
            review, proposal, scope, challenge, signed_proof,
            separately_pinned_public_key, separately_pinned_operator_id, now_ms)
        != ExactCode::cryptographically_consistent_no_authority)
        return Code::invalid_exact_message_signature;

    const std::string nonce_sha256 =
        local_goose_dialogue_responder_retry_nonce_fingerprint(challenge.nonce_hex);
    if (nonce_sha256.empty()) return Code::invalid_expected_reservation;

    const auto expected =
        make_local_goose_dialogue_responder_retry_challenge_reservation(
            expected_request_id, challenge.challenge_id, proposal.proposal_id,
            scope.scope_id, scope.lineage_id, nonce_sha256,
            challenge.issued_at_ms, challenge.expires_at_ms);
    if (!expected) return Code::invalid_expected_reservation;

    try {
        // Open is read-only and existing-only; schema/owner/mode/symlink and
        // SQLite companions are rejected by the isolated ledger constructor.
        Ledger ledger(existing_ledger_path, OpenMode::existing_read_only);
        if (!ledger.is_read_only()
            || ledger.schema_version()
                != local_goose_dialogue_responder_retry_challenge_ledger_schema)
            return Code::ledger_unavailable_or_unsafe;
        const std::optional<Record> actual =
            ledger.find_challenge(challenge.challenge_id);
        if (!actual) return Code::reservation_missing;
        if (!same_reservation(actual->reservation, *expected))
            return Code::reservation_mismatch;
        if (actual->state != State::reserved
            || actual->evidence_sha256.has_value())
            return Code::reservation_already_used_or_uncertain;
        // Still not one-time access: another reader can repeat this check,
        // or the row can change immediately after this observation.
        return Code::consistent_reserved_no_authority;
    } catch (...) {
        return Code::ledger_unavailable_or_unsafe;
    }
}

} // namespace gaudere_agent
