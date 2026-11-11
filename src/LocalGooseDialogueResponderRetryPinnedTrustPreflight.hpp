#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_PINNED_TRUST_PREFLIGHT_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_PINNED_TRUST_PREFLIGHT_HPP

#include "LocalGooseDialogueResponderRetryOperatorTrustFloorLedger.hpp"
#include "LocalGooseDialogueResponderRetryRootPin.hpp"

#include <cstdint>
#include <string>

namespace gaudere_agent {

// Source-only, read-only inspection of three pre-existing pieces of evidence.
// A matching protected root pin, root-signed policy, and monotonic local floor
// are NOT real operator enrollment, human presence or execution authority.
enum class LocalGooseDialogueResponderRetryPinnedTrustPreflightCode : std::uint8_t {
    consistent_active_no_authority,
    consistent_revoked_no_authority,
    mutable_floor_forbidden,
    floor_unavailable_or_wrong_root,
    invalid_expected_operator_key,
    invalid_signed_policy_or_time,
    signed_policy_below_floor,
    signed_policy_wrong_operator_key
};

// Caller MUST supply a root handle previously loaded with an independently
// established expected fingerprint. Caller-provided trust anchors, local
// storage, and wall clock are not authenticated by this function.
// The ledger MUST have been opened existing_read_only, not as a writer.
// No store creation, generation change, key enrollment, Task, ACK, dispatch.
[[nodiscard]] LocalGooseDialogueResponderRetryPinnedTrustPreflightCode
inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
    const LocalGooseDialogueResponderRetryRootPin& verified_root_pin,
    const LocalGooseDialogueResponderRetryTrustFloorLedger& existing_read_only_floor,
    const std::string& candidate_signed_snapshot,
    const std::string& independently_expected_operator_key_fingerprint,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
