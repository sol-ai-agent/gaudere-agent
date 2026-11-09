#include "LocalGooseDialogueResponderRetrySignedProofConsumer.hpp"

#include "Sha256.hpp"

#include <cstdint>
#include <string>

namespace gaudere_agent {
namespace {

using Result = LocalGooseDialogueResponderRetrySignedProofConsumeCode;
using Ledger = LocalGooseDialogueResponderRetryChallengeLedger;
using OpenMode = LocalGooseDialogueResponderRetryChallengeLedgerOpenMode;
using LedgerCode = LocalGooseDialogueResponderRetryChallengeLedgerCode;
using ProofCode = LocalGooseDialogueResponderRetryExactMessageProofCode;

void append(std::string& out, const std::string& value)
{
    out += std::to_string(value.size()) + ":" + value + "|";
}

std::string evidence_digest(
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& proof,
    const LocalGooseDialogueResponderRetryChallengeReservation& reservation,
    const std::string& exact_signing_payload)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-retry-consumed-exact-proof.v1|";
    append(canonical, reservation.reservation_id);
    append(canonical, reservation.request_id);
    append(canonical, reservation.challenge_id);
    append(canonical, reservation.scope_id);
    append(canonical, reservation.nonce_sha256);
    append(canonical, review.review_id);
    append(canonical, review.message_sha256);
    append(canonical, exact_signing_payload); // includes raw message bytes
    append(canonical, proof.operator_id);
    append(canonical, proof.decision);
    append(canonical, proof.key_fingerprint);
    append(canonical, proof.signature_hex);
    return sha256_hex(canonical);
}

} // namespace

Result consume_local_goose_dialogue_responder_retry_signed_proof_once(
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
    try {
        // A signature from an arbitrary caller-chosen key does not imply
        // enrollment, true human presence, trusted time or consent. The
        // caller's pinned key must never be accepted as its own trust source.
        if (inspect_local_goose_dialogue_responder_retry_exact_message_signature(
                review, proposal, scope, challenge, signed_proof,
                separately_pinned_public_key, separately_pinned_operator_id,
                now_ms) != ProofCode::cryptographically_consistent_no_authority)
            return Result::invalid_signed_message_or_expired;

        const std::string nonce_fingerprint =
            local_goose_dialogue_responder_retry_nonce_fingerprint(
                challenge.nonce_hex);
        if (nonce_fingerprint.empty())
            return Result::invalid_expected_reservation;

        const auto expected =
            make_local_goose_dialogue_responder_retry_challenge_reservation(
                expected_request_id, challenge.challenge_id, proposal.proposal_id,
                scope.scope_id, scope.lineage_id, nonce_fingerprint,
                challenge.issued_at_ms, challenge.expires_at_ms);
        if (!expected)
            return Result::invalid_expected_reservation;

        const auto payload =
            local_goose_dialogue_responder_retry_exact_message_signing_payload(
                review, proposal, scope, challenge,
                signed_proof.operator_id, signed_proof.decision);
        if (!payload)
            return Result::invalid_signed_message_or_expired;

        const std::string evidence =
            evidence_digest(review, signed_proof, *expected, *payload);

        // Unlike the earlier independent read-only inspector, the ledger's
        // immutable identity comparison and burn are one BEGIN IMMEDIATE
        // transaction. It is still NOT atomic with a future authority store,
        // and even a successful burn cannot create a runtime effect.
        Ledger ledger(existing_ledger_path, OpenMode::existing_writer);
        if (ledger.is_read_only()
            || ledger.schema_version()
                != local_goose_dialogue_responder_retry_challenge_ledger_schema)
            return Result::ledger_unavailable_or_commit_uncertain;
        switch (ledger.burn_if_exact(*expected, evidence)) {
            case LedgerCode::accepted:
                return Result::burned_no_authority;
            case LedgerCode::conflict:
                return Result::already_burned_uncertain_or_mismatch;
            case LedgerCode::invalid:
                return Result::invalid_expected_reservation;
            default:
                // A commit might have happened before the caller lost its
                // result. The only safe default is manual operator review.
                return Result::ledger_unavailable_or_commit_uncertain;
        }
    } catch (...) {
        return Result::ledger_unavailable_or_commit_uncertain;
    }
}

} // namespace gaudere_agent
