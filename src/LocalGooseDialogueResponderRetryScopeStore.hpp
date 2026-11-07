#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_SCOPE_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_SCOPE_STORE_HPP

#include "LocalGooseDialogueResponderRetryAuthority.hpp"

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_retry_scope_schema = 1;
inline constexpr const char* local_goose_dialogue_responder_retry_scope_prefix =
    "cognition.local-goose-dialogue.responder-retry-scope.v1:";

/**
 * Immutable proposal of an evidence scope; explicitly NOT an authority grant.
 * A scope contains no message, speaker permission, lease, decision, intent,
 * disposition or dispatch operation. A later independent, human-gated service
 * must revalidate the live authoritative evidence before using this scope.
 */
struct LocalGooseDialogueResponderRetryScope {
    std::string scope_id;
    std::string request_id;
    std::string lineage_id;
    std::string consumer_id;
    std::string event_id;
    std::string result_sha256;
    std::string task_id;
    std::string thread_alias;
    std::string observed_speaker_kind;
    std::string observed_speaker_id;
    std::uint32_t retry_generation = 0;
    std::uint64_t completion_sequence = 0;
    std::uint64_t expected_head_revision = 0;
    std::int64_t prepared_at_ms = -1;
};

[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryScope>
make_local_goose_dialogue_responder_retry_scope(
    const std::string& request_id,
    const LocalGooseDialogueResponderRetryLineage& lineage,
    std::int64_t prepared_at_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_retry_scope(
    const LocalGooseDialogueResponderRetryScope& scope) noexcept;

enum class LocalGooseDialogueResponderRetryScopeOpenMode : std::uint8_t {
    create_new_isolated = 0,
    existing_read_only = 1,
    existing_writer = 2
};

enum class LocalGooseDialogueResponderRetryScopeResult : std::uint8_t {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueResponderRetryScopeWrite {
    LocalGooseDialogueResponderRetryScopeResult result =
        LocalGooseDialogueResponderRetryScopeResult::unavailable;
    std::optional<LocalGooseDialogueResponderRetryScope> scope;
};

/**
 * Separate schema-v1 SQLite file, never the historical responder SQLite or
 * the generation-1 audit-lineage sidecar. No migrations or runtime wiring.
 *
 * create_new_isolated requires an absent file, O_EXCL, owner mode 0600.
 * existing_read_only never creates or writes and rejects companions.
 * existing_writer is for explicit test/isolated replay; no production caller.
 */
class LocalGooseDialogueResponderRetryScopeStore {
public:
    LocalGooseDialogueResponderRetryScopeStore(
        const std::string& path,
        LocalGooseDialogueResponderRetryScopeOpenMode mode);
    ~LocalGooseDialogueResponderRetryScopeStore();

    LocalGooseDialogueResponderRetryScopeStore(
        const LocalGooseDialogueResponderRetryScopeStore&) = delete;
    LocalGooseDialogueResponderRetryScopeStore& operator=(
        const LocalGooseDialogueResponderRetryScopeStore&) = delete;

    [[nodiscard]] int schema_version() const noexcept { return schema_version_; }
    [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryScope>
    find_by_request(const std::string& request_id) const;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryScope>
    find_by_lineage(const std::string& lineage_id) const;

    [[nodiscard]] LocalGooseDialogueResponderRetryScopeWrite
    stage_scope(const LocalGooseDialogueResponderRetryScope& scope);

private:
    sqlite3* database_ = nullptr;
    int schema_version_ = 0;
    bool read_only_ = false;
};

} // namespace gaudere_agent

#endif
