#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ELIGIBILITY_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ELIGIBILITY_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_responder_first_retry_lineage_id =
    "cognition.local-goose-dialogue.responder-retry-lineage.v1:"
    "da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca";

enum class LocalGooseDialogueResponderRetryEligibilityCode : std::uint8_t {
    evidence_consistent_no_authority,
    lineage_absent,
    conflict,
    unavailable
};

struct LocalGooseDialogueResponderRetryEligibilityStatus {
    LocalGooseDialogueResponderRetryEligibilityCode code =
        LocalGooseDialogueResponderRetryEligibilityCode::unavailable;
    std::string detail;
};

/**
 * Code-only read of the exact recovered sequence-4 generation-1 audit lineage.
 *
 * Inspect does not prepare, activate, mutate, dispatch or ACK anything. A
 * consistent audit lineage is NOT permission to create new grant/lease/decision
 * or to submit a Task. All inputs are authoritative, already-open store handles.
 *
 * IMPORTANT: The existing retry-store constructor may initialize an absent
 * sidecar. A future caller must first prove the file already exists and open
 * the store through a separately audited read-only path. This service is NOT
 * wired to production and must never be exposed as a production command using
 * the current write-capable constructor.
 */
class LocalGooseDialogueResponderRetryEligibilityService {
public:
    LocalGooseDialogueResponderRetryEligibilityService(
        const LocalGooseDialogueCompletionFeedStore& completion,
        const LocalGooseDialogueThreadStore& thread,
        const LocalGooseDialogueResponderStore& responder,
        const LocalGooseDialogueResponderAuthorityStore& authority,
        const gaudere::work::TaskStore& tasks,
        const LocalGooseDialogueResponderRetryStore& retry);

    [[nodiscard]] LocalGooseDialogueResponderRetryEligibilityStatus
    inspect_generation_1(std::int64_t now_ms) const;

private:
    const LocalGooseDialogueCompletionFeedStore& completion_;
    const LocalGooseDialogueThreadStore& thread_;
    const LocalGooseDialogueResponderStore& responder_;
    const LocalGooseDialogueResponderAuthorityStore& authority_;
    const gaudere::work::TaskStore& tasks_;
    const LocalGooseDialogueResponderRetryStore& retry_;
};

} // namespace gaudere_agent

#endif
