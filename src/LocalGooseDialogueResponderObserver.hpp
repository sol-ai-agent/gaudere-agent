#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_OBSERVER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_OBSERVER_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialogueResponderObserverClassification : std::uint8_t {
    no_event = 0,
    structural_system = 1,
    structural_stale = 2,
    current_human = 3,
    intervention_owned = 4,
    reconciliation_pending = 5,
    ambiguous = 6
};

enum class LocalGooseDialogueResponderObserverAction : std::uint8_t {
    none = 0,
    disarmed = 1,
    held = 2,
    acknowledged = 3,
    reconciled = 4,
    manual_review = 5,
    unavailable = 6
};

struct LocalGooseDialogueResponderObserverConfig {
    std::string consumer_id = "system-responder-v1";
    std::string thread_alias = "main";
    std::string structural_authority_source = "observer-policy-v1";
    bool observation_armed = false;
};

struct LocalGooseDialogueResponderObserverStatus {
    bool observation_armed = false;
    std::string consumer_id;
    std::string thread_alias;
    std::optional<std::uint64_t> consumer_last_sequence;
    std::optional<LocalGooseDialogueCompletionEvent> next_event;
    std::optional<LocalGooseDialogueThreadHead> preferred_head;
    std::optional<LocalGooseDialogueResponderDisposition> disposition;
    LocalGooseDialogueResponderObserverClassification classification =
        LocalGooseDialogueResponderObserverClassification::no_event;
    std::string detail;
};

struct LocalGooseDialogueResponderObserverStepResult {
    LocalGooseDialogueResponderObserverAction action =
        LocalGooseDialogueResponderObserverAction::none;
    LocalGooseDialogueResponderObserverStatus status;
    std::string detail;
};

/**
 * Exact typed authorization to consume one current human preferred-head event
 * without an intervention. No absence-of-lease or model decision can synthesize
 * this authority.
 */
struct LocalGooseDialogueResponderExplicitDecline {
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::uint64_t thread_revision = 0;
    std::string task_id;
    std::uint64_t expected_head_revision = 0;
    std::string expected_head_task_id;
    std::string authority_source;
};

using LocalGooseDialogueResponderObserverClock =
    std::function<std::int64_t()>;

/**
 * Durable responder observation policy.
 *
 * The observer owns no provider, model, Task execution, responder dispatch,
 * lease creation, intent creation, wake, stimulus, cycle or shell authority.
 * When explicitly armed it may retire only exact structural non-intervention
 * events, or an exact current-human event carrying a typed explicit decline.
 * A normal current-human preferred-head event is held without ACK.
 *
 * step() processes at most one completion-event cause. It recovers prepared
 * non-intervention dispositions before classifying a new event, including the
 * crash window after feed ACK but before disposition reconciliation.
 */
class LocalGooseDialogueResponderObserver {
public:
    LocalGooseDialogueResponderObserver(
        LocalGooseDialogueCompletionFeedStore& feed_store,
        LocalGooseDialogueThreadStore& thread_store,
        LocalGooseDialogueResponderStore& responder_store,
        LocalGooseDialogueResponderObserverConfig config,
        LocalGooseDialogueResponderObserverClock clock);

    /** Read-only status; never ACKs or mutates any sidecar. */
    [[nodiscard]] LocalGooseDialogueResponderObserverStatus inspect() const;

    /**
     * Recover one pending non-intervention disposition or classify/retire one
     * exact structural event. Disarmed mode is read-only.
     */
    [[nodiscard]] LocalGooseDialogueResponderObserverStepResult step();

    /**
     * Apply one separately authorized explicit decline to the exact current
     * human preferred-head event. This never creates a lease or intent.
     */
    [[nodiscard]] LocalGooseDialogueResponderObserverStepResult decline(
        const LocalGooseDialogueResponderExplicitDecline& authorization);

private:
    [[nodiscard]] LocalGooseDialogueResponderObserverStatus inspect_current()
        const;

    LocalGooseDialogueCompletionFeedStore& feed_store_;
    LocalGooseDialogueThreadStore& thread_store_;
    LocalGooseDialogueResponderStore& responder_store_;
    LocalGooseDialogueResponderObserverConfig config_;
    LocalGooseDialogueResponderObserverClock clock_;
};

} // namespace gaudere_agent

#endif
