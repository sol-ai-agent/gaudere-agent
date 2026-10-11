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

enum class LocalGooseDialogueResponderStoreOpenMode : std::uint8_t {
    preserve_schema = 0,
    allow_schema_upgrade = 1
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

struct LocalGooseDialogueResponderDispositionWrite {
    LocalGooseDialogueResponderStoreResult result =
        LocalGooseDialogueResponderStoreResult::invalid;
    std::optional<LocalGooseDialogueResponderDisposition> disposition;
    std::string detail;
};

/**
 * Durable bounded authority ledger for system-authored dialogue interventions
 * and durable completion-event disposition causes.
 *
 * This store owns no Task execution, model, provider, scheduler, stimulus,
 * network, shell, preferred-head or completion-feed ACK authority. It persists
 * only finite responder leases, immutable intervention intents and immutable
 * event dispositions plus their reconciliation state.
 *
 * Opening an existing schema-v1 sidecar preserves it by default. The v1 -> v2
 * schema upgrade is a distinct explicit operation selected with
 * allow_schema_upgrade; ordinary code presence or service restart must not
 * mutate an existing responder sidecar schema.
 */
class LocalGooseDialogueResponderStore {
public:
    explicit LocalGooseDialogueResponderStore(
        const std::string& path,
        LocalGooseDialogueResponderStoreOpenMode open_mode =
            LocalGooseDialogueResponderStoreOpenMode::preserve_schema);
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

    [[nodiscard]] std::optional<LocalGooseDialogueResponderDisposition>
    find_disposition(const std::string& disposition_id) const;

    [[nodiscard]] std::optional<LocalGooseDialogueResponderDisposition>
    find_disposition_for_event(
        const std::string& consumer_id,
        const std::string& event_id) const;

    [[nodiscard]] std::vector<LocalGooseDialogueResponderDisposition>
    dispositions_in_state(
        LocalGooseDialogueResponderDispositionState state,
        std::size_t limit = 64) const;

    /**
     * Persist one immutable cause for how a responder consumer will treat one
     * completion event. This method never ACKs the completion feed.
     */
    [[nodiscard]] LocalGooseDialogueResponderDispositionWrite
    prepare_disposition(
        const LocalGooseDialogueResponderDisposition& disposition);

    /**
     * Bind a prepared intervention disposition to an already-prepared exact
     * responder intent. This method neither dispatches nor ACKs anything.
     */
    [[nodiscard]] LocalGooseDialogueResponderDispositionWrite
    bind_disposition_intent(
        const std::string& disposition_id,
        const std::string& intent_id,
        std::int64_t bound_at_ms);

    /**
     * Record that an external typed observer has reconciled the exact feed ACK
     * for a non-intervention disposition. This method itself has no feed ACK
     * authority.
     */
    [[nodiscard]] LocalGooseDialogueResponderDispositionWrite
    acknowledge_disposition(
        const std::string& disposition_id,
        std::int64_t reconciled_at_ms);

    /**
     * Record that an intervention disposition's already-bound intent and exact
     * trigger-event ACK have been externally reconciled. No dispatch/ACK occurs
     * in this method.
     */
    [[nodiscard]] LocalGooseDialogueResponderDispositionWrite
    complete_disposition(
        const std::string& disposition_id,
        std::int64_t reconciled_at_ms);

    [[nodiscard]] LocalGooseDialogueResponderDispositionWrite
    terminalize_disposition(
        const std::string& disposition_id,
        const std::string& reason,
        std::int64_t terminal_at_ms);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
