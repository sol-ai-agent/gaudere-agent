#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_FLOOR_LEDGER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_OPERATOR_TRUST_FLOOR_LEDGER_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_retry_trust_floor_schema = 1;

// An INERT local counter only. Its root fingerprint MUST be compared to a
// separately enrolled human-controlled trust anchor; the caller, Drive,
// the proposed signed policy and this SQLite file cannot enroll that root.
// A floor checked after an authenticated root-signed revocation can prevent
// acceptance of an older signed ACTIVE policy after normal restart.
struct LocalGooseDialogueResponderRetryTrustFloorRecord {
    std::string root_fingerprint;
    std::uint64_t minimum_generation = 0;
    std::string last_transition_sha256; // opaque evidence, NOT a signature
};

class LocalGooseDialogueResponderRetryRootPin;
class LocalGooseDialogueResponderRetryTrustFloorLedger;
enum class LocalGooseDialogueResponderRetryRevocationCommitCode : std::uint8_t;

// Only the bridge with verified Ed25519 REVOKED evidence may call the
// ledger's private compare-and-advance implementation.
[[nodiscard]] LocalGooseDialogueResponderRetryRevocationCommitCode
commit_local_goose_dialogue_responder_retry_verified_revocation(
    LocalGooseDialogueResponderRetryTrustFloorLedger& ledger,
    const std::string& candidate_signed_snapshot,
    const LocalGooseDialogueResponderRetryRootPin& enrolled_root_pin,
    const std::string& independently_trusted_operator_key_fingerprint,
    std::int64_t trusted_now_ms);

enum class LocalGooseDialogueResponderRetryTrustFloorOpenMode : std::uint8_t {
    create_new_isolated,
    existing_read_only,
    existing_writer
};

enum class LocalGooseDialogueResponderRetryTrustFloorCode : std::uint8_t {
    advanced_no_authority,
    stale_or_conflicting_floor,
    invalid,
    unavailable_or_commit_uncertain
};

// This isolated SQLite ledger does not authenticate the human controller,
// issue a challenge, approve a message or dispatch any Task. Its private
// mutation entry point is accessible only to the signed REVOKED verifier.
// The caller must still supply genuinely independent enrolled trust anchors.
//
// All updates compare root fingerprint AND prior generation under one
// BEGIN IMMEDIATE transaction. No decrement, reset, duplicate advance,
// implicit creation on opening or automatic retry after unknown commit.
class LocalGooseDialogueResponderRetryTrustFloorLedger {
public:
    LocalGooseDialogueResponderRetryTrustFloorLedger(
        const std::string& trusted_absolute_path,
        LocalGooseDialogueResponderRetryTrustFloorOpenMode mode,
        const std::string& externally_verified_initial_root_fingerprint = {});
    ~LocalGooseDialogueResponderRetryTrustFloorLedger();

    LocalGooseDialogueResponderRetryTrustFloorLedger(
        const LocalGooseDialogueResponderRetryTrustFloorLedger&) = delete;
    LocalGooseDialogueResponderRetryTrustFloorLedger& operator=(
        const LocalGooseDialogueResponderRetryTrustFloorLedger&) = delete;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryTrustFloorRecord>
    inspect(const std::string& independently_trusted_root_fingerprint) const;

    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }
private:
    friend LocalGooseDialogueResponderRetryRevocationCommitCode
    commit_local_goose_dialogue_responder_retry_verified_revocation(
        LocalGooseDialogueResponderRetryTrustFloorLedger& ledger,
        const std::string& candidate_signed_snapshot,
        const LocalGooseDialogueResponderRetryRootPin& enrolled_root_pin,
        const std::string& independently_trusted_operator_key_fingerprint,
        std::int64_t trusted_now_ms);

    // Never exposed directly to callers: unsigned supplied hashes/generations
    // cannot mutate a trust floor through the public C++ API.
    [[nodiscard]] LocalGooseDialogueResponderRetryTrustFloorCode
    advance_after_external_signature_review(
        const std::string& independently_trusted_root_fingerprint,
        std::uint64_t expected_prior_generation,
        std::uint64_t root_signed_new_generation,
        const std::string& externally_verified_snapshot_sha256);

    sqlite3* database_ = nullptr;
    bool read_only_ = true;
};

} // namespace gaudere_agent

#endif
