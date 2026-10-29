#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_STORE_HPP

#include "LocalGooseDialogueResponderRetryAuthority.hpp"

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

enum class LocalGooseDialogueResponderRetryStoreResult : std::uint8_t {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueResponderRetryLineageWrite {
    LocalGooseDialogueResponderRetryStoreResult result =
        LocalGooseDialogueResponderRetryStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderRetryLineage> lineage;
    std::string detail;
};

/**
 * Dedicated append-only retry-lineage sidecar.
 *
 * This store is intentionally separate from the schema-v3 responder authority
 * sidecar. It cannot mutate historical grant/lease/decision/intent/disposition
 * rows and exposes no Task, runtime, provider, completion-feed ACK, dispatch,
 * lease creation or authority activation surface.
 */
class LocalGooseDialogueResponderRetryStore {
public:
    explicit LocalGooseDialogueResponderRetryStore(const std::string& path);
    ~LocalGooseDialogueResponderRetryStore();

    LocalGooseDialogueResponderRetryStore(
        const LocalGooseDialogueResponderRetryStore&) = delete;
    LocalGooseDialogueResponderRetryStore& operator=(
        const LocalGooseDialogueResponderRetryStore&) = delete;

    [[nodiscard]] int schema_version() const noexcept { return schema_version_; }

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryLineage>
    find_lineage(const std::string& lineage_id) const;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryLineage>
    find_by_request(const std::string& request_id) const;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderRetryLineage>
    find_for_event_generation(
        const std::string& consumer_id,
        const std::string& event_id,
        std::uint32_t retry_generation) const;

    [[nodiscard]] LocalGooseDialogueResponderRetryLineageWrite prepare_lineage(
        const LocalGooseDialogueResponderRetryLineage& lineage);

private:
    sqlite3* database_ = nullptr;
    int schema_version_ = 0;
};

} // namespace gaudere_agent

#endif
