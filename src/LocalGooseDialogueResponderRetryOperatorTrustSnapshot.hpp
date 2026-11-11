#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_SNAPSHOT_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_SNAPSHOT_HPP

#include "LocalGooseDialogueResponderRetryOperatorKeyPin.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// An offline trust-policy snapshot, signed by a SEPARATELY enrolled trust
// root. The root key/fingerprint and minimum accepted generation MUST be
// pinned in operator-controlled state, NEVER sourced from the candidate.
// Neither the snapshot nor a matching signature is an execution capability.
inline constexpr const char* local_goose_dialogue_responder_retry_trust_snapshot_header =
    "gaudere-retry-operator-trust-snapshot.v1\n"
    "operator_id=bertrand\n";

enum class LocalGooseDialogueResponderRetryTrustSnapshotCode : std::uint8_t {
    cryptographically_consistent_no_authority,
    invalid_root_or_independent_trust,
    invalid_snapshot_format,
    invalid_root_signature,
    revoked_or_stale_or_outside_validity,
    operator_pin_or_exact_message_invalid
};

// Cryptographic evidence ONLY, verified by a separately enrolled root
// supplied from a future trusted operator controller. Does not authorize an
// effect or establish human presence. A signed REVOKED tombstone is returned
// here (unlike the ACTIVE-only exact-message inspector below).
enum class LocalGooseDialogueResponderRetrySignedTrustStatus : std::uint8_t {
    active,
    revoked
};

struct LocalGooseDialogueResponderRetryVerifiedSignedTrustPolicy {
    LocalGooseDialogueResponderRetrySignedTrustStatus status;
    std::string operator_key_fingerprint;
    std::uint64_t generation = 0;
    std::string snapshot_sha256;
};

[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryVerifiedSignedTrustPolicy>
verify_local_goose_dialogue_responder_retry_signed_trust_policy(
    const std::string& candidate_signed_snapshot,
    const std::array<unsigned char, 32>& separately_enrolled_root_public_key,
    const std::string& independently_trusted_root_fingerprint,
    std::int64_t trusted_now_ms);

// A root-signed local snapshot can accept or revoke an exact key fingerprint.
// A trusted minimum generation rejects an old, signed ACTIVE record after
// a future revocation; that minimum must be persisted independently.
// The snapshot is a *non-authoritative* check: no root enrollment,
// human presence, state mutation, one-shot consume or runtime dispatch.
[[nodiscard]] LocalGooseDialogueResponderRetryTrustSnapshotCode
inspect_local_goose_dialogue_responder_retry_operator_trust_snapshot(
    const std::string& candidate_signed_snapshot,
    const std::array<unsigned char, 32>& separately_enrolled_root_public_key,
    const std::string& independently_trusted_root_fingerprint,
    std::uint64_t independently_trusted_minimum_generation,
    const std::string& trusted_absolute_operator_key_pin_path,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& operator_proof,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
