#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_AUTHORITY_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_AUTHORITY_HPP

#include <cstdint>
#include <string>

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_responder_retry_sidecar_schema = 1;
inline constexpr std::uint32_t local_goose_dialogue_responder_retry_generation = 1;
inline constexpr const char* local_goose_dialogue_responder_retry_lineage_prefix =
    "cognition.local-goose-dialogue.responder-retry-lineage.v1:";

/**
 * Immutable lineage for one explicitly authorized generation-1 retry of one
 * exact completion event after the historical responder authority attempt
 * became unusable before dispatch.
 *
 * The lineage itself grants no authority. It only binds exact event/head
 * evidence to the historical grant/lease/decision/intent/disposition chain.
 * A later typed authority service must separately prove those historical states
 * before admitting any new grant, lease, intent, submission or feed ACK.
 *
 * This v1 contract intentionally models no second retry. If generation 1 also
 * fails, a later version and a new explicit gate are required.
 */
struct LocalGooseDialogueResponderRetryLineage {
    std::string lineage_id;
    std::string request_id;
    std::string consumer_id;
    std::string thread_alias;
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::uint64_t thread_revision = 0;
    std::string task_id;
    std::string observed_speaker_kind;
    std::string observed_speaker_id;

    std::uint32_t retry_generation = 0;

    std::string historical_grant_id;
    std::string historical_lease_id;
    std::string historical_decision_id;
    std::string historical_intent_id;
    std::string historical_disposition_id;
    std::string historical_grant_terminal_reason;
    std::string historical_lease_terminal_reason;
    std::string historical_decision_terminal_reason;
    std::string historical_intent_terminal_reason;
    std::string historical_disposition_terminal_reason;

    std::int64_t created_at_ms = 0;
};

[[nodiscard]] LocalGooseDialogueResponderRetryLineage
make_local_goose_dialogue_responder_retry_lineage(
    const std::string& request_id,
    const std::string& consumer_id,
    const std::string& thread_alias,
    std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& observed_speaker_id,
    const std::string& historical_grant_id,
    const std::string& historical_lease_id,
    const std::string& historical_decision_id,
    const std::string& historical_intent_id,
    const std::string& historical_disposition_id,
    const std::string& historical_grant_terminal_reason,
    const std::string& historical_lease_terminal_reason,
    const std::string& historical_decision_terminal_reason,
    const std::string& historical_intent_terminal_reason,
    const std::string& historical_disposition_terminal_reason,
    std::int64_t created_at_ms);

[[nodiscard]] bool valid_local_goose_dialogue_responder_retry_lineage(
    const LocalGooseDialogueResponderRetryLineage& lineage) noexcept;

} // namespace gaudere_agent

#endif
