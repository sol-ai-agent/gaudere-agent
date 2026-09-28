#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_STORE_HPP

#include "LocalGooseDialogueResponder.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace gaudere_agent {

enum class LocalGooseDialogueResponderStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueResponderLeaseWrite {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderLease> lease;
    std::string detail;
};

struct LocalGooseDialogueResponderIntentWrite {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderIntent> intent;
    std::string detail;
};

/**
 * Durable bounded authority ledger for system-authored dialogue interventions.
 *
 * This store owns no Task execution, model, provider, scheduler, stimulus,
 * network, shell, preferred-head or completion-feed authority. It persists only
 * finite responder leases and immutable intervention intents plus reconciliation
 * state.
 */
class LocalGooseDialogueResponderStore {
public:
    explicit LocalGooseDialogueResponderStore(const std::string& path);
    ~LocalGooseDialogueResponderStore();

    LocalGooseDialogueResponderStore(
        const LocalGooseDialogueResponderStore&) = delete;
    LocalGooseDialogueResponderStore& operator=(
        const LocalGooseDialogueResponderStore&) = delete;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderLease> find_lease(
        const std::string& lease_id) const;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderLease>
    find_active_lease(const std::string& thread_alias) const;

    [[nodiscard]] LocalGooseDialogueResponderLeaseWrite create_lease(
        const LocalGooseDialogueResponderLease& lease);

    [[nodiscard]] LocalGooseDialogueResponderLeaseWrite close_lease(
        const std::string& lease_id,
        LocalGooseDialogueResponderLeaseState terminal_state,
        const std::string& reason,
        std::int64_t terminal_at_ms);

    [[nodiscard]] std::optional<LocalGooseDialogueResponderIntent> find_intent(
        const std::string& intent_id) const;

    [[nodiscard]] std::vector<LocalGooseDialogueResponderIntent>
    intents_in_state(
        LocalGooseDialogueResponderIntentState state,
        std::size_t limit = 64) const;

    [[nodiscard]] LocalGooseDialogueResponderIntentWrite prepare_intent(
        const LocalGooseDialogueResponderIntent& intent);

    /**
     * Reconcile a prepared intent with an already-canonical V3 Task and account
     * exactly one lease turn. This does not submit the Task.
     */
    [[nodiscard]] LocalGooseDialogueResponderIntentWrite commit_submission(
        const std::string& intent_id,
        const std::string& task_id,
        std::uint64_t resulting_thread_revision,
        std::int64_t committed_at_ms);

    [[nodiscard]] LocalGooseDialogueResponderIntentWrite complete_intent(
        const std::string& intent_id,
        std::int64_t completed_at_ms);

    [[nodiscard]] LocalGooseDialogueResponderIntentWrite terminalize_intent(
        const std::string& intent_id,
        LocalGooseDialogueResponderIntentState terminal_state,
        const std::string& reason);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
