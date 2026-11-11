#include "LocalGooseDialogueResponderRetryVerifiedRevocation.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {
namespace {
using Result = LocalGooseDialogueResponderRetryRevocationCommitCode;
using FloorCode = LocalGooseDialogueResponderRetryTrustFloorCode;
using Status = LocalGooseDialogueResponderRetrySignedTrustStatus;
} // namespace

Result commit_local_goose_dialogue_responder_retry_verified_revocation(
    LocalGooseDialogueResponderRetryTrustFloorLedger& ledger,
    const std::string& candidate_signed_snapshot,
    const LocalGooseDialogueResponderRetryRootPin& enrolled_root_pin,
    const std::string& independently_trusted_operator_key_fingerprint,
    const std::int64_t trusted_now_ms)
{
    try {
        // Signature validation happens BEFORE obtaining any mutable ledger
        // state or entering the SQLite transaction. All four exact policy
        // properties are covered by the root signature. Do not parse an
        // unverified generation or digest for effect.
        const auto verified =
            verify_local_goose_dialogue_responder_retry_signed_trust_policy(
                candidate_signed_snapshot,
                enrolled_root_pin.public_key(),
                enrolled_root_pin.fingerprint(), trusted_now_ms);
        if (!verified) return Result::invalid_signed_policy_or_root;
        if (verified->operator_key_fingerprint
                != independently_trusted_operator_key_fingerprint)
            return Result::wrong_operator_key;
        if (verified->status != Status::revoked)
            return Result::not_a_revocation;

        // The floor is independent, and its root must already match.
        // A concurrent update after this read fails the internal BEGIN
        // IMMEDIATE compare-and-swap, never an automatic retry.
        const auto current =
            ledger.inspect(enrolled_root_pin.fingerprint());
        if (!current) return Result::unavailable_or_commit_uncertain;
        if (verified->generation <= current->minimum_generation)
            return Result::stale_or_conflicting_floor;

        const auto result = ledger.advance_after_external_signature_review(
            enrolled_root_pin.fingerprint(),
            current->minimum_generation,
            verified->generation,
            verified->snapshot_sha256);
        switch (result) {
            case FloorCode::advanced_no_authority:
                return Result::advanced_no_authority;
            case FloorCode::stale_or_conflicting_floor:
                return Result::stale_or_conflicting_floor;
            case FloorCode::invalid:
                return Result::invalid_signed_policy_or_root;
            case FloorCode::unavailable_or_commit_uncertain:
                return Result::unavailable_or_commit_uncertain;
        }
    } catch (...) {
        return Result::unavailable_or_commit_uncertain;
    }
    return Result::unavailable_or_commit_uncertain;
}

} // namespace gaudere_agent
