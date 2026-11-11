#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_VERIFIED_REVOCATION_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_VERIFIED_REVOCATION_HPP

#include "LocalGooseDialogueResponderRetryOperatorTrustFloorLedger.hpp"
#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"
#include "LocalGooseDialogueResponderRetryRootPin.hpp"

#include <cstdint>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialogueResponderRetryRevocationCommitCode : std::uint8_t {
    advanced_no_authority,
    invalid_signed_policy_or_root,
    wrong_operator_key,
    not_a_revocation,
    stale_or_conflicting_floor,
    unavailable_or_commit_uncertain
};

// The ONE public C++ update path for the isolated trust floor.
// Strictly verifies the exact Ed25519 ROOT-SIGNED REVOKED tombstone, root
// fingerprint, independently pinned operator key fingerprint and validity
// before CAS. Never trusts the snapshot to define its own root or key.
// No runtime activation, user enrollment, consent or grant authority.
// The independently trusted values and wall clock remain an explicit
// unresolved operator-controlled boundary; an attacker controlling them
// could still authorize their own root. SQLite also cannot prevent an
// attacker restoring an older full database backup.
[[nodiscard]] LocalGooseDialogueResponderRetryRevocationCommitCode
commit_local_goose_dialogue_responder_retry_verified_revocation(
    LocalGooseDialogueResponderRetryTrustFloorLedger& ledger,
    const std::string& candidate_signed_snapshot,
    const LocalGooseDialogueResponderRetryRootPin& enrolled_root_pin,
    const std::string& independently_trusted_operator_key_fingerprint,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
