#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ADMISSION_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ADMISSION_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialogueResponderRetryAdmissionResult : std::uint8_t {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueResponderRetryAdmissionRequest {
    std::string request_id;
    std::string consumer_id;
    std::string historical_grant_id;
    std::string historical_lease_id;
    std::string historical_decision_id;
    std::string historical_intent_id;
    std::string historical_disposition_id;
};

struct LocalGooseDialogueResponderRetryAdmissionWrite {
    LocalGooseDialogueResponderRetryAdmissionResult result =
        LocalGooseDialogueResponderRetryAdmissionResult::invalid;
    std::optional<LocalGooseDialogueResponderRetryLineage> lineage;
    std::string detail;
};

using LocalGooseDialogueResponderRetryAdmissionClock =
    std::function<std::int64_t()>;

/**
 * Read-authoritative, write-narrow admission service for one generation-1
 * responder retry lineage.
 *
 * Historical responder/authority, completion-feed, preferred-head and Task
 * stores are held internally as const references. The only mutable store is the
 * dedicated append-only retry sidecar, and the only mutation requested from it
 * is prepare_lineage().
 *
 * A successfully prepared lineage is audit evidence only. It creates no grant,
 * lease, decision, intent, disposition, Task, preferred-head update or feed ACK
 * and grants no dispatch authority.
 */
class LocalGooseDialogueResponderRetryAdmissionService {
public:
    LocalGooseDialogueResponderRetryAdmissionService(
        LocalGooseDialogueCompletionFeedStore& completion_store,
        LocalGooseDialogueThreadStore& thread_store,
        LocalGooseDialogueResponderStore& responder_store,
        LocalGooseDialogueResponderAuthorityStore& authority_store,
        gaudere::work::TaskStore& task_store,
        LocalGooseDialogueResponderRetryStore& retry_store,
        LocalGooseDialogueResponderRetryAdmissionClock clock);

    [[nodiscard]] LocalGooseDialogueResponderRetryAdmissionWrite
    prepare_lineage(
        const LocalGooseDialogueResponderRetryAdmissionRequest& request);

private:
    const LocalGooseDialogueCompletionFeedStore& completion_store_;
    const LocalGooseDialogueThreadStore& thread_store_;
    const LocalGooseDialogueResponderStore& responder_store_;
    const LocalGooseDialogueResponderAuthorityStore& authority_store_;
    const gaudere::work::TaskStore& task_store_;
    LocalGooseDialogueResponderRetryStore& retry_store_;
    LocalGooseDialogueResponderRetryAdmissionClock clock_;
};

} // namespace gaudere_agent

#endif
