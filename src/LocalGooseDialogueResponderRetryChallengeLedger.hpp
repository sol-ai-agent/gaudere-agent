#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_CHALLENGE_LEDGER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_CHALLENGE_LEDGER_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_retry_challenge_ledger_schema = 1;
inline constexpr const char* local_goose_dialogue_responder_retry_reservation_prefix =
    "cognition.local-goose-dialogue.responder-retry-challenge-reservation.v1:";

// An inert, immutable registration record. The nonce fingerprint and the
// 'burned' state do not authenticate the nonce issuer, signer, human consent,
// or any effect. This ledger is NOT an authority or replay-safe dispatcher.
struct LocalGooseDialogueResponderRetryChallengeReservation {
    std::string reservation_id;
    std::string request_id;
    std::string challenge_id;
    std::string proposal_id;
    std::string scope_id;
    std::string lineage_id;
    std::string nonce_sha256;
    std::uint32_t retry_generation = 0;
    std::int64_t issued_at_ms = -1;
    std::int64_t expires_at_ms = -1;
};

enum class LocalGooseDialogueResponderRetryChallengeState : std::uint8_t {
    reserved = 0,
    burned = 1,
    manual_review = 2
};

struct LocalGooseDialogueResponderRetryChallengeRecord {
    LocalGooseDialogueResponderRetryChallengeReservation reservation;
    LocalGooseDialogueResponderRetryChallengeState state =
        LocalGooseDialogueResponderRetryChallengeState::reserved;
    std::optional<std::string> evidence_sha256;
};

[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryChallengeReservation>
make_local_goose_dialogue_responder_retry_challenge_reservation(
    const std::string& request_id,
    const std::string& challenge_id,
    const std::string& proposal_id,
    const std::string& scope_id,
    const std::string& lineage_id,
    const std::string& nonce_sha256,
    std::int64_t issued_at_ms,
    std::int64_t expires_at_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_retry_challenge_reservation(
    const LocalGooseDialogueResponderRetryChallengeReservation& reservation) noexcept;

enum class LocalGooseDialogueResponderRetryChallengeLedgerOpenMode : std::uint8_t {
    create_new_isolated = 0,
    existing_read_only = 1,
    existing_writer = 2
};

enum class LocalGooseDialogueResponderRetryChallengeLedgerCode : std::uint8_t {
    accepted,
    exact_duplicate_reserved,
    conflict,
    invalid,
    unavailable
};

// A *burn* only makes the nonce unusable. It is not a signature verification,
// a grant, or an approval. Repeated burns fail closed, even with identical
// request/evidence. No transition can unburn a row.
class LocalGooseDialogueResponderRetryChallengeLedger {
public:
    LocalGooseDialogueResponderRetryChallengeLedger(
        const std::string& path,
        LocalGooseDialogueResponderRetryChallengeLedgerOpenMode mode);
    ~LocalGooseDialogueResponderRetryChallengeLedger();

    LocalGooseDialogueResponderRetryChallengeLedger(
        const LocalGooseDialogueResponderRetryChallengeLedger&) = delete;
    LocalGooseDialogueResponderRetryChallengeLedger& operator=(
        const LocalGooseDialogueResponderRetryChallengeLedger&) = delete;

    [[nodiscard]] int schema_version() const noexcept { return schema_version_; }
    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryChallengeRecord>
    find_challenge(const std::string& challenge_id) const;

    [[nodiscard]] LocalGooseDialogueResponderRetryChallengeLedgerCode
    reserve(const LocalGooseDialogueResponderRetryChallengeReservation& reservation);

    [[nodiscard]] LocalGooseDialogueResponderRetryChallengeLedgerCode
    burn(const std::string& challenge_id,
         const std::string& request_id,
         const std::string& evidence_sha256);

    [[nodiscard]] LocalGooseDialogueResponderRetryChallengeLedgerCode
    mark_uncertain(const std::string& challenge_id,
                   const std::string& request_id);

private:
    sqlite3* database_ = nullptr;
    int schema_version_ = 0;
    bool read_only_ = false;
};

} // namespace gaudere_agent

#endif
