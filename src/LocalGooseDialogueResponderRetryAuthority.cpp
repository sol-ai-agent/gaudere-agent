#include "LocalGooseDialogueResponderRetryAuthority.hpp"

#include "Sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace gaudere_agent {
namespace {

constexpr const char* completion_event_prefix =
    "cognition.local-goose-dialogue.completion.v1:";
constexpr const char* v3_task_prefix = "cognition.local-goose-dialogue.v3:";
constexpr const char* grant_prefix =
    "cognition.local-goose-dialogue.responder-grant.v1:";
constexpr const char* decision_prefix =
    "cognition.local-goose-dialogue.responder-decision.v1:";
constexpr const char* intent_prefix =
    "cognition.local-goose-dialogue.responder-intent.v1:";
constexpr const char* disposition_prefix =
    "cognition.local-goose-dialogue.responder-disposition.v1:";
constexpr std::size_t max_identifier_bytes = 128;
constexpr std::size_t max_reason_bytes = 1024;

bool safe_identifier(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_identifier_bytes) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_'
            || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& value, const char* prefix) noexcept
{
    const std::string p{prefix};
    return value.size() == p.size() + 64
        && value.compare(0, p.size(), p) == 0
        && lowercase_sha256(value.substr(p.size()));
}

bool bounded_reason(const std::string& value) noexcept
{
    return !value.empty() && value.size() <= max_reason_bytes;
}

void append(std::string& out, const std::string& value)
{
    out += std::to_string(value.size());
    out += ':';
    out += value;
    out += '|';
}

void append(std::string& out, const std::uint64_t value)
{
    out += std::to_string(value);
    out += '|';
}

std::string lineage_digest(
    const LocalGooseDialogueResponderRetryLineage& lineage)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-retry-lineage.v1|";
    append(canonical, lineage.request_id);
    append(canonical, lineage.consumer_id);
    append(canonical, lineage.thread_alias);
    append(canonical, lineage.completion_sequence);
    append(canonical, lineage.event_id);
    append(canonical, lineage.result_sha256);
    append(canonical, lineage.thread_revision);
    append(canonical, lineage.task_id);
    append(canonical, lineage.observed_speaker_kind);
    append(canonical, lineage.observed_speaker_id);
    append(canonical, static_cast<std::uint64_t>(lineage.retry_generation));
    append(canonical, lineage.previous_retry_lineage_id.value_or(""));
    append(canonical, lineage.historical_grant_id);
    append(canonical, lineage.historical_lease_id);
    append(canonical, lineage.historical_decision_id);
    append(canonical, lineage.historical_intent_id);
    append(canonical, lineage.historical_disposition_id);
    append(canonical, lineage.historical_grant_terminal_reason);
    append(canonical, lineage.historical_lease_terminal_reason);
    append(canonical, lineage.historical_decision_terminal_reason);
    append(canonical, lineage.historical_intent_terminal_reason);
    append(canonical, lineage.historical_disposition_terminal_reason);
    return sha256_hex(canonical);
}

} // namespace

LocalGooseDialogueResponderRetryLineage
make_local_goose_dialogue_responder_retry_lineage(
    const std::string& request_id,
    const std::string& consumer_id,
    const std::string& thread_alias,
    const std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    const std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& observed_speaker_id,
    const std::uint32_t retry_generation,
    std::optional<std::string> previous_retry_lineage_id,
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
    const std::int64_t created_at_ms)
{
    LocalGooseDialogueResponderRetryLineage lineage;
    lineage.request_id = request_id;
    lineage.consumer_id = consumer_id;
    lineage.thread_alias = thread_alias;
    lineage.completion_sequence = completion_sequence;
    lineage.event_id = event_id;
    lineage.result_sha256 = result_sha256;
    lineage.thread_revision = thread_revision;
    lineage.task_id = task_id;
    lineage.observed_speaker_kind = "human";
    lineage.observed_speaker_id = observed_speaker_id;
    lineage.retry_generation = retry_generation;
    lineage.previous_retry_lineage_id = std::move(previous_retry_lineage_id);
    lineage.historical_grant_id = historical_grant_id;
    lineage.historical_lease_id = historical_lease_id;
    lineage.historical_decision_id = historical_decision_id;
    lineage.historical_intent_id = historical_intent_id;
    lineage.historical_disposition_id = historical_disposition_id;
    lineage.historical_grant_terminal_reason = historical_grant_terminal_reason;
    lineage.historical_lease_terminal_reason = historical_lease_terminal_reason;
    lineage.historical_decision_terminal_reason = historical_decision_terminal_reason;
    lineage.historical_intent_terminal_reason = historical_intent_terminal_reason;
    lineage.historical_disposition_terminal_reason =
        historical_disposition_terminal_reason;
    lineage.created_at_ms = created_at_ms;
    lineage.lineage_id =
        std::string{local_goose_dialogue_responder_retry_lineage_prefix}
        + lineage_digest(lineage);
    return lineage;
}

bool valid_local_goose_dialogue_responder_retry_lineage(
    const LocalGooseDialogueResponderRetryLineage& lineage) noexcept
{
    if (!prefixed_sha256(
            lineage.lineage_id,
            local_goose_dialogue_responder_retry_lineage_prefix)
        || !safe_identifier(lineage.request_id)
        || !safe_identifier(lineage.consumer_id)
        || !safe_identifier(lineage.thread_alias)
        || lineage.completion_sequence == 0
        || lineage.completion_sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(lineage.event_id, completion_event_prefix)
        || !lowercase_sha256(lineage.result_sha256)
        || lineage.thread_revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(lineage.task_id, v3_task_prefix)
        || lineage.observed_speaker_kind != "human"
        || !safe_identifier(lineage.observed_speaker_id)
        || lineage.retry_generation == 0
        || lineage.retry_generation
            > local_goose_dialogue_responder_max_retry_generation
        || !prefixed_sha256(lineage.historical_grant_id, grant_prefix)
        || !safe_identifier(lineage.historical_lease_id)
        || !prefixed_sha256(lineage.historical_decision_id, decision_prefix)
        || !prefixed_sha256(lineage.historical_intent_id, intent_prefix)
        || !prefixed_sha256(
            lineage.historical_disposition_id, disposition_prefix)
        || !bounded_reason(lineage.historical_grant_terminal_reason)
        || !bounded_reason(lineage.historical_lease_terminal_reason)
        || !bounded_reason(lineage.historical_decision_terminal_reason)
        || !bounded_reason(lineage.historical_intent_terminal_reason)
        || !bounded_reason(lineage.historical_disposition_terminal_reason)
        || lineage.created_at_ms < 0) {
        return false;
    }

    if (lineage.retry_generation == 1) {
        if (lineage.previous_retry_lineage_id) return false;
    } else {
        if (!lineage.previous_retry_lineage_id
            || !prefixed_sha256(
                *lineage.previous_retry_lineage_id,
                local_goose_dialogue_responder_retry_lineage_prefix)) {
            return false;
        }
    }

    return lineage.lineage_id
        == std::string{local_goose_dialogue_responder_retry_lineage_prefix}
            + lineage_digest(lineage);
}

} // namespace gaudere_agent
