#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_STORE_HPP

#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderStore.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace gaudere_agent {

enum class LocalGooseDialogueResponderAuthorityStoreOpenMode : std::uint8_t {
    preserve_schema = 0,
    allow_schema_upgrade = 1
};

struct LocalGooseDialogueResponderGrantWrite {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderGrant> grant;
    std::string detail;
};

struct LocalGooseDialogueResponderDecisionWrite {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderDecision> decision;
    std::string detail;
};

/**
 * Durable decision-authority ledger layered on the responder sidecar.
 *
 * Schema v2 is deliberately preserved by default. The only path that creates
 * grant/decision tables is an explicit v2 -> v3 open with
 * allow_schema_upgrade. No lease, intent, disposition, completion cursor,
 * Task, provider or dialogue state is created by opening or migrating this
 * store.
 */
class LocalGooseDialogueResponderAuthorityStore {
public:
    explicit LocalGooseDialogueResponderAuthorityStore(
        const std::string& path,
        LocalGooseDialogueResponderAuthorityStoreOpenMode open_mode =
            LocalGooseDialogueResponderAuthorityStoreOpenMode::preserve_schema);
    ~LocalGooseDialogueResponderAuthorityStore();

    LocalGooseDialogueResponderAuthorityStore(
        const LocalGooseDialogueResponderAuthorityStore&) = delete;
    LocalGooseDialogueResponderAuthorityStore& operator=(
        const LocalGooseDialogueResponderAuthorityStore&) = delete;

    [[nodiscard]] int schema_version() const noexcept { return schema_version_; }

    [[nodiscard]] std::optional<LocalGooseDialogueResponderGrant> find_grant(
        const std::string& grant_id) const;
    [[nodiscard]] std::optional<LocalGooseDialogueResponderGrant>
    find_grant_by_request(const std::string& request_id) const;
    [[nodiscard]] std::vector<LocalGooseDialogueResponderGrant> grants_in_state(
        LocalGooseDialogueResponderGrantState state,
        std::size_t limit = 64) const;

    [[nodiscard]] LocalGooseDialogueResponderGrantWrite prepare_grant(
        const LocalGooseDialogueResponderGrant& grant);
    [[nodiscard]] LocalGooseDialogueResponderGrantWrite activate_grant(
        const std::string& grant_id,
        const std::string& lease_id,
        std::int64_t activated_at_ms);
    [[nodiscard]] LocalGooseDialogueResponderGrantWrite terminalize_grant(
        const std::string& grant_id,
        LocalGooseDialogueResponderGrantState terminal_state,
        const std::string& reason,
        std::int64_t terminal_at_ms);

    [[nodiscard]] std::optional<LocalGooseDialogueResponderDecision>
    find_decision(const std::string& decision_id) const;
    [[nodiscard]] std::optional<LocalGooseDialogueResponderDecision>
    find_decision_by_request(const std::string& request_id) const;
    [[nodiscard]] std::optional<LocalGooseDialogueResponderDecision>
    find_decision_for_event(
        const std::string& consumer_id,
        const std::string& event_id) const;
    [[nodiscard]] std::vector<LocalGooseDialogueResponderDecision>
    decisions_in_state(
        LocalGooseDialogueResponderDecisionState state,
        std::size_t limit = 64) const;

    [[nodiscard]] LocalGooseDialogueResponderDecisionWrite prepare_decision(
        const LocalGooseDialogueResponderDecision& decision);
    [[nodiscard]] LocalGooseDialogueResponderDecisionWrite bind_decision_intent(
        const std::string& decision_id,
        const std::string& intent_id,
        std::int64_t bound_at_ms);
    [[nodiscard]] LocalGooseDialogueResponderDecisionWrite complete_decision(
        const std::string& decision_id,
        std::int64_t completed_at_ms);
    [[nodiscard]] LocalGooseDialogueResponderDecisionWrite terminalize_decision(
        const std::string& decision_id,
        LocalGooseDialogueResponderDecisionState terminal_state,
        const std::string& reason,
        std::int64_t terminal_at_ms);

private:
    [[nodiscard]] bool authority_schema_ready() const noexcept
    {
        return schema_version_
            == local_goose_dialogue_responder_authority_sidecar_schema;
    }

    sqlite3* database_ = nullptr;
    int schema_version_ = 0;
};

} // namespace gaudere_agent

#endif
