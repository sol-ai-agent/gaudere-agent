#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_SNAPSHOT_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_SNAPSHOT_HPP

#include "LocalGooseDialogueResponderRetryOperatorKeyPin.hpp"

#include <array>
#include <cstdint>
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
