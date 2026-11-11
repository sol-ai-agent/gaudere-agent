#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ROOT_OWNED_ANCHOR_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ROOT_OWNED_ANCHOR_HPP

#include "LocalGooseDialogueResponderRetryOperatorTrustFloorLedger.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// Built-in pathname, never supplied by Drive, Task, API caller or a snapshot.
// A root-owned file at this location is an OS isolation predicate, NOT proof
// of a human enrollment ceremony. No such file is installed by this stage.
inline constexpr const char* local_goose_dialogue_responder_retry_root_owned_anchor_path =
    "/etc/gaudere/retry-operator-root-anchor.v1";
inline constexpr const char* local_goose_dialogue_responder_retry_root_owned_anchor_header =
    "gaudere-retry-root-owned-anchor.v1\n";

struct LocalGooseDialogueResponderRetryRootOwnedAnchorRecord {
    std::string root_pin_path;
    std::string root_fingerprint;
    std::string operator_key_fingerprint;
    std::uint64_t anchor_epoch = 0;
};

// No arguments or automatic enrollment; strictly reads the fixed root-owned
// public file with openat/O_NOFOLLOW/fstat and rejects execution as root.
// This is source-only with no production wiring; installed configuration
// provenance, root compromise and machine rollback remain outside its scope.
[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryRootOwnedAnchorRecord>
load_local_goose_dialogue_responder_retry_root_owned_anchor();

enum class LocalGooseDialogueResponderRetryRootOwnedPreflightCode : std::uint8_t {
    source_missing_or_untrusted_no_authority,
    root_pin_invalid_no_authority,
    signed_policy_or_floor_invalid_no_authority,
    consistent_active_no_authority,
    consistent_revoked_no_authority
};

// Does not accept candidate-selected root path, expected root fingerprint or
// operator key fingerprint; those can only be read from the fixed root-owned
// source. Existing floor must be read-only. Never issues consent or runtime
// authority, and is NOT an enrollment controller.
[[nodiscard]] LocalGooseDialogueResponderRetryRootOwnedPreflightCode
inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
    const LocalGooseDialogueResponderRetryTrustFloorLedger& existing_read_only_floor,
    const std::string& candidate_signed_snapshot,
    std::int64_t trusted_now_ms);

} // namespace gaudere_agent

#endif
