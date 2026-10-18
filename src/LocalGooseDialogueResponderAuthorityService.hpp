#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_SERVICE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_AUTHORITY_SERVICE_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialogueResponderAuthorityServiceCode : std::uint8_t {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable,
    disabled
};

struct LocalGooseDialogueResponderAuthorityServiceConfig {
    bool authority_enabled = false;
    std::string authority_source = "local-typed-operator-v1";
    std::string consumer_id = "system-responder-v1";
};

struct LocalGooseDialogueResponderGrantPrepareRequest {
    std::string request_id;
    std::string thread_alias;
    std::string speaker_id;
    std::uint32_t allowed_message_kinds = 0;
    std::string purpose;
    std::uint64_t max_system_turns = 0;
    std::int64_t max_lease_ttl_ms = 0;
    std::int64_t min_interval_ms = 0;
    std::int64_t grant_ttl_ms = 0;
};

struct LocalGooseDialogueResponderGrantServiceResult {
    LocalGooseDialogueResponderAuthorityServiceCode result =
        LocalGooseDialogueResponderAuthorityServiceCode::invalid;
    std::optional<LocalGooseDialogueResponderGrant> grant;
    std::optional<LocalGooseDialogueResponderLease> lease;
    std::string detail;
};

struct LocalGooseDialogueResponderDecisionPrepareRequest {
    std::string request_id;
    std::string grant_id;
    std::string lease_id;
    std::string consumer_id;
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::string thread_alias;
    std::uint64_t thread_revision = 0;
    std::string task_id;
    std::string observed_speaker_kind;
    std::string observed_speaker_id;
    std::uint64_t expected_head_revision = 0;
    std::string expected_head_task_id;
    std::string message_kind;
    std::string message;
    std::int64_t intent_ttl_ms = 0;
};

struct LocalGooseDialogueResponderDecisionServiceResult {
    LocalGooseDialogueResponderAuthorityServiceCode result =
        LocalGooseDialogueResponderAuthorityServiceCode::invalid;
    std::optional<LocalGooseDialogueResponderDecision> decision;
    std::optional<LocalGooseDialogueResponderIntent> intent;
    std::string detail;
};

struct LocalGooseDialogueResponderAuthorityStatus {
    bool authority_enabled = false;
    std::string authority_source;
    std::string consumer_id;
    std::optional<std::uint64_t> consumer_last_sequence;
    std::optional<LocalGooseDialogueCompletionEvent> next_event;
    std::optional<LocalGooseDialogueThreadHead> head;
    std::optional<LocalGooseDialogueResponderLease> active_lease;
    std::optional<LocalGooseDialogueResponderGrant> prepared_grant;
    std::optional<LocalGooseDialogueResponderGrant> activated_grant;
    std::optional<LocalGooseDialogueResponderDecision> event_decision;
    std::string detail;
};

using LocalGooseDialogueResponderAuthorityClock =
    std::function<std::int64_t()>;

/**
 * Explicit typed authority service for durable responder grants and decisions.
 *
 * The service is disabled by default. It owns no polling/thread and never
 * dispatches an intent. inspect() is read-only. Authority mutations occur only
 * through explicit calls while authority_enabled=true.
 *
 * Grant activation creates/reconciles exactly one deterministic finite lease.
 * Decision binding may prepare/reconcile an intent+intervention disposition
 * through the existing dispatcher, but never submits a Task or ACKs a trigger.
 */
class LocalGooseDialogueResponderAuthorityService {
public:
    LocalGooseDialogueResponderAuthorityService(
        LocalGooseDialogueCompletionFeedStore& completion_store,
        LocalGooseDialogueThreadStore& thread_store,
        LocalGooseDialogueResponderStore& responder_store,
        LocalGooseDialogueResponderAuthorityStore& authority_store,
        LocalGooseDialogueResponderDispatcher& dispatcher,
        LocalGooseDialogueResponderAuthorityServiceConfig config,
        LocalGooseDialogueResponderAuthorityClock clock);

    [[nodiscard]] LocalGooseDialogueResponderAuthorityStatus inspect(
        const std::string& thread_alias) const;

    [[nodiscard]] LocalGooseDialogueResponderGrantServiceResult prepare_grant(
        const LocalGooseDialogueResponderGrantPrepareRequest& request);

    [[nodiscard]] LocalGooseDialogueResponderGrantServiceResult activate_grant(
        const std::string& grant_id);

    [[nodiscard]] LocalGooseDialogueResponderDecisionServiceResult
    prepare_decision(
        const LocalGooseDialogueResponderDecisionPrepareRequest& request);

    [[nodiscard]] LocalGooseDialogueResponderDecisionServiceResult bind_decision(
        const std::string& decision_id);

private:
    LocalGooseDialogueCompletionFeedStore& completion_store_;
    LocalGooseDialogueThreadStore& thread_store_;
    LocalGooseDialogueResponderStore& responder_store_;
    LocalGooseDialogueResponderAuthorityStore& authority_store_;
    LocalGooseDialogueResponderDispatcher& dispatcher_;
    LocalGooseDialogueResponderAuthorityServiceConfig config_;
    LocalGooseDialogueResponderAuthorityClock clock_;
};

} // namespace gaudere_agent

#endif
