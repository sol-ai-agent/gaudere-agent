#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_ANCHOR_RECEIPT_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_ANCHOR_RECEIPT_HPP

#include "LocalGooseDialogueResponderRetryRootOwnedAnchor.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace gaudere_agent {

// Pure, source-only and non-authoritative HOST-RECEIPT inspector.
// The receipt is Ed25519 signed by a DIFFERENT proposed host identity,
// not the retry trust root or the exact-message operator signing key.
// A host signer is trusted ONLY if its key/context/floor were independently
// pinned by a future authenticated host enrollment controller. This reader
// does not authenticate how those inputs were obtained.
inline constexpr const char* local_goose_dialogue_responder_retry_host_receipt_header =
    "gaudere-retry-host-anchor-receipt.v1\n";
inline constexpr const char* local_goose_dialogue_responder_retry_host_receipt_domain =
    "gaudere-retry-host-anchor-receipt-signature.v1|";

enum class LocalGooseDialogueResponderRetryHostReceiptCode : std::uint8_t {
    cryptographically_consistent_no_authority,
    invalid_independent_host_trust,
    invalid_receipt_format,
    invalid_host_signature,
    anchor_or_host_context_mismatch,
    stale_or_outside_validity
};

// Compare a bounded canonical host-signed receipt against an already-read
// public root-owned manifest (#289). This does NOT establish that manifest
// came from the real host rather than a spoofed container mount namespace.
// "independently_..." inputs are preconditions, not attestations performed
// here. There is no root key enrollment, approval, Task/ACK, or effect.
[[nodiscard]] LocalGooseDialogueResponderRetryHostReceiptCode
inspect_local_goose_dialogue_responder_retry_host_anchor_receipt(
    const std::string& candidate_exact_signed_receipt,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    std::uint64_t independently_trusted_minimum_anchor_epoch,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
