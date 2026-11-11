#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CHALLENGE_ONCE_LEDGER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CHALLENGE_ONCE_LEDGER_HPP

#include "LocalGooseDialogueResponderRetryHostChallengeProof.hpp"

#include <array>
#include <cstdint>
#include <string>

struct sqlite3;

namespace gaudere_agent {

// Fixed, exact SQLite v1 schema for a PRE-SEEDED synthetic challenge ledger.
// No challenge issuance, creation or automatic repair is exposed here.
inline constexpr int local_goose_dialogue_responder_retry_host_once_schema_version = 1;
inline constexpr const char* local_goose_dialogue_responder_retry_host_once_table =
    "local_goose_dialogue_responder_retry_host_challenge_once";
inline constexpr const char* local_goose_dialogue_responder_retry_host_once_schema_sql =
    "CREATE TABLE local_goose_dialogue_responder_retry_host_challenge_once("
    "challenge_sha256 TEXT PRIMARY KEY NOT NULL,"
    "request_context_sha256 TEXT NOT NULL,"
    "host_context_sha256 TEXT NOT NULL,"
    "anchor_epoch INTEGER NOT NULL CHECK(anchor_epoch>=1),"
    "expires_ms INTEGER NOT NULL CHECK(expires_ms>0),"
    "consumed INTEGER NOT NULL CHECK(consumed IN(0,1)),"
    "accepted_proof_sha256 TEXT NOT NULL)";

enum class LocalGooseDialogueResponderRetryHostOnceCode : std::uint8_t {
    verified_challenge_marked_consumed_no_authority,
    missing_or_already_consumed_no_authority,
    mismatched_or_expired_issued_challenge_no_authority,
    invalid_signed_proof_no_authority,
    ledger_unavailable_or_commit_uncertain_no_authority
};

class LocalGooseDialogueResponderRetryHostChallengeOnceLedger;

// Atomically mark ONE pre-issued challenge as consumed after verifying the
// *exact* v1 host receipt and v2 host challenge signature. The caller still
// supplies allegedly independent trust values. Successful commit is local
// accounting ONLY, NOT operator consent, host enrollment or execution grant.
//
// The ledger does NOT issue/mint challenges. Its rows must be pre-issued by
// a future independently authenticated host controller. No such controller
// is implemented, and existing local SQLite is not rollback-safe.
[[nodiscard]] LocalGooseDialogueResponderRetryHostOnceCode
verify_and_mark_local_goose_dialogue_responder_retry_host_challenge_once(
    LocalGooseDialogueResponderRetryHostChallengeOnceLedger& ledger,
    const std::string& exact_v1_host_signed_receipt,
    const std::string& exact_v2_host_challenge_proof,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    std::uint64_t independently_trusted_minimum_anchor_epoch,
    const std::string& independently_expected_challenge_sha256,
    const std::string& independently_expected_request_context_sha256,
    std::int64_t trusted_now_ms);

// Existing-only writer; safe private parent/file required. Opening never
// creates or repairs a file, issues a challenge or resets consumed history.
// No public SQL writer/consume entry point or post-commit retry.
class LocalGooseDialogueResponderRetryHostChallengeOnceLedger final {
public:
    explicit LocalGooseDialogueResponderRetryHostChallengeOnceLedger(
        const std::string& trusted_existing_absolute_path);
    ~LocalGooseDialogueResponderRetryHostChallengeOnceLedger();

    LocalGooseDialogueResponderRetryHostChallengeOnceLedger(
        const LocalGooseDialogueResponderRetryHostChallengeOnceLedger&) = delete;
    LocalGooseDialogueResponderRetryHostChallengeOnceLedger& operator=(
        const LocalGooseDialogueResponderRetryHostChallengeOnceLedger&) = delete;

private:
    friend LocalGooseDialogueResponderRetryHostOnceCode
    verify_and_mark_local_goose_dialogue_responder_retry_host_challenge_once(
        LocalGooseDialogueResponderRetryHostChallengeOnceLedger&,
        const std::string&, const std::string&,
        const std::array<unsigned char, 32>&,
        const std::string&, const std::string&,
        const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord&,
        std::uint64_t, const std::string&, const std::string&, std::int64_t);

    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent
#endif
