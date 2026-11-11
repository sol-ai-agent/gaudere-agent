#include "LocalGooseDialogueResponderRetryPinnedTrustPreflight.hpp"

#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"

#include <cstdint>
#include <string>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryPinnedTrustPreflightCode;
bool lower_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

} // namespace

Code inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
    const LocalGooseDialogueResponderRetryRootPin& verified_root_pin,
    const LocalGooseDialogueResponderRetryTrustFloorLedger& existing_read_only_floor,
    const std::string& candidate_signed_snapshot,
    const std::string& independently_expected_operator_key_fingerprint,
    const std::int64_t trusted_now_ms)
{
    try {
        // Never silently downgrade to a writable/newly-created floor.
        if (!existing_read_only_floor.is_read_only())
            return Code::mutable_floor_forbidden;
        if (!lower_sha256(independently_expected_operator_key_fingerprint))
            return Code::invalid_expected_operator_key;

        const auto floor = existing_read_only_floor.inspect(
            verified_root_pin.fingerprint());
        if (!floor || floor->minimum_generation == 0)
            return Code::floor_unavailable_or_wrong_root;

        const auto policy =
            verify_local_goose_dialogue_responder_retry_signed_trust_policy(
                candidate_signed_snapshot,
                verified_root_pin.public_key(),
                verified_root_pin.fingerprint(),
                trusted_now_ms);
        if (!policy)
            return Code::invalid_signed_policy_or_time;

        // The floor is durable local accounting only, not a trusted
        // hardware-backed anti-rollback anchor.
        if (policy->generation < floor->minimum_generation)
            return Code::signed_policy_below_floor;
        if (policy->operator_key_fingerprint
                != independently_expected_operator_key_fingerprint)
            return Code::signed_policy_wrong_operator_key;

        return policy->status
                == LocalGooseDialogueResponderRetrySignedTrustStatus::revoked
            ? Code::consistent_revoked_no_authority
            : Code::consistent_active_no_authority;
    } catch (...) {
        return Code::floor_unavailable_or_wrong_root;
    }
}

} // namespace gaudere_agent
