#include "LocalGooseDialogueResponder.hpp"

#include "Sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace gaudere_agent {
namespace {

constexpr const char* completion_event_prefix =
    "cognition.local-goose-dialogue.completion.v1:";
constexpr const char* v2_task_prefix = "cognition.local-goose-dialogue.v2:";
constexpr const char* v3_task_prefix = "cognition.local-goose-dialogue.v3:";
constexpr std::size_t max_identifier_bytes = 128;
constexpr std::size_t max_purpose_bytes = 1024;
constexpr std::size_t max_message_bytes = 4096;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::int64_t max_duration_ms = 24LL * 60 * 60 * 1000;
constexpr std::uint64_t max_system_turns = 8;

bool safe_identifier(const std::string& value,
                     const std::size_t max_bytes = max_identifier_bytes) noexcept
{
    if (value.empty() || value.size() > max_bytes) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
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

bool dialogue_task_id(const std::string& value) noexcept
{
    return prefixed_sha256(value, v2_task_prefix)
        || prefixed_sha256(value, v3_task_prefix);
}

bool v3_task_id(const std::string& value) noexcept
{
    return prefixed_sha256(value, v3_task_prefix);
}

bool bounded_nonempty(const std::string& value, const std::size_t max_bytes) noexcept
{
    return !value.empty() && value.size() <= max_bytes;
}

bool valid_lease_state(const LocalGooseDialogueResponderLeaseState state) noexcept
{
    using State = LocalGooseDialogueResponderLeaseState;
    return state == State::active || state == State::exhausted
        || state == State::expired || state == State::revoked
        || state == State::manual_review;
}

bool valid_intent_state(const LocalGooseDialogueResponderIntentState state) noexcept
{
    using State = LocalGooseDialogueResponderIntentState;
    return state == State::prepared || state == State::submitted
        || state == State::completed || state == State::conflict
        || state == State::expired || state == State::manual_review;
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

void append(std::string& out, const std::int64_t value)
{
    out += std::to_string(value);
    out += '|';
}

std::string intent_digest(const LocalGooseDialogueResponderIntent& intent)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-intent.v1|";
    append(canonical, intent.lease_id);
    append(canonical, intent.thread_alias);
    append(canonical, intent.completion_sequence);
    append(canonical, intent.event_id);
    append(canonical, intent.result_sha256);
    append(canonical, intent.triggering_task_id);
    append(canonical, intent.expected_thread_revision);
    append(canonical, intent.expected_head_task_id);
    append(canonical, intent.speaker_id);
    append(canonical, intent.message_kind);
    append(canonical, intent.message);
    return sha256_hex(canonical);
}

} // namespace

std::uint32_t local_goose_dialogue_responder_message_kind_bit(
    const std::string& message_kind) noexcept
{
    if (message_kind == "dialogue") {
        return local_goose_dialogue_responder_message_dialogue;
    }
    if (message_kind == "feedback") {
        return local_goose_dialogue_responder_message_feedback;
    }
    if (message_kind == "intervention") {
        return local_goose_dialogue_responder_message_intervention;
    }
    if (message_kind == "observation") {
        return local_goose_dialogue_responder_message_observation;
    }
    return 0;
}

bool valid_local_goose_dialogue_responder_lease(
    const LocalGooseDialogueResponderLease& lease) noexcept
{
    if (!safe_identifier(lease.lease_id)
        || !safe_identifier(lease.thread_alias)
        || lease.speaker_kind != "system"
        || !safe_identifier(lease.speaker_id)
        || lease.allowed_message_kinds == 0
        || (lease.allowed_message_kinds
            & ~local_goose_dialogue_responder_message_all) != 0
        || !bounded_nonempty(lease.purpose, max_purpose_bytes)
        || lease.max_system_turns == 0
        || lease.max_system_turns > max_system_turns
        || lease.turns_committed > lease.max_system_turns
        || lease.issued_at_ms < 0
        || lease.expires_at_ms <= lease.issued_at_ms
        || lease.expires_at_ms - lease.issued_at_ms > max_duration_ms
        || lease.min_interval_ms < 0 || lease.min_interval_ms > max_duration_ms
        || lease.next_eligible_at_ms < lease.issued_at_ms
        || !valid_lease_state(lease.state)
        || lease.terminal_reason.size() > max_reason_bytes) {
        return false;
    }

    using State = LocalGooseDialogueResponderLeaseState;
    if (lease.state == State::active) {
        return lease.turns_committed < lease.max_system_turns
            && lease.terminal_reason.empty() && !lease.terminal_at_ms;
    }
    if (!lease.terminal_at_ms || *lease.terminal_at_ms < lease.issued_at_ms
        || lease.terminal_reason.empty()) {
        return false;
    }
    if (lease.state == State::exhausted) {
        return lease.turns_committed == lease.max_system_turns;
    }
    return true;
}

LocalGooseDialogueResponderIntent make_local_goose_dialogue_responder_intent(
    const std::string& lease_id,
    const std::string& thread_alias,
    const std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    const std::string& triggering_task_id,
    const std::uint64_t expected_thread_revision,
    const std::string& expected_head_task_id,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message,
    const std::int64_t created_at_ms,
    const std::int64_t expires_at_ms)
{
    LocalGooseDialogueResponderIntent intent;
    intent.lease_id = lease_id;
    intent.thread_alias = thread_alias;
    intent.completion_sequence = completion_sequence;
    intent.event_id = event_id;
    intent.result_sha256 = result_sha256;
    intent.triggering_task_id = triggering_task_id;
    intent.expected_thread_revision = expected_thread_revision;
    intent.expected_head_task_id = expected_head_task_id;
    intent.speaker_id = speaker_id;
    intent.message_kind = message_kind;
    intent.message = message;
    intent.created_at_ms = created_at_ms;
    intent.expires_at_ms = expires_at_ms;
    const auto digest = intent_digest(intent);
    intent.intent_id =
        std::string{local_goose_dialogue_responder_intent_prefix} + digest;
    intent.request_id =
        std::string{local_goose_dialogue_responder_request_prefix} + digest;
    return intent;
}

bool valid_local_goose_dialogue_responder_intent(
    const LocalGooseDialogueResponderIntent& intent) noexcept
{
    if (!prefixed_sha256(
            intent.intent_id, local_goose_dialogue_responder_intent_prefix)
        || !safe_identifier(intent.lease_id)
        || !safe_identifier(intent.thread_alias)
        || intent.completion_sequence == 0
        || intent.completion_sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(intent.event_id, completion_event_prefix)
        || !lowercase_sha256(intent.result_sha256)
        || !dialogue_task_id(intent.triggering_task_id)
        || intent.expected_thread_revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - 1)
        || !dialogue_task_id(intent.expected_head_task_id)
        || intent.speaker_kind != "system"
        || !safe_identifier(intent.speaker_id)
        || local_goose_dialogue_responder_message_kind_bit(intent.message_kind) == 0
        || !bounded_nonempty(intent.message, max_message_bytes)
        || !safe_identifier(intent.request_id)
        || intent.created_at_ms < 0
        || intent.expires_at_ms <= intent.created_at_ms
        || intent.expires_at_ms - intent.created_at_ms > max_duration_ms
        || !valid_intent_state(intent.state)
        || intent.terminal_reason.size() > max_reason_bytes) {
        return false;
    }

    const auto digest = intent_digest(intent);
    if (intent.intent_id
            != std::string{local_goose_dialogue_responder_intent_prefix} + digest
        || intent.request_id
            != std::string{local_goose_dialogue_responder_request_prefix} + digest) {
        return false;
    }

    const bool any_submission = !intent.submitted_task_id.empty()
        || intent.submitted_thread_revision || intent.committed_at_ms;
    const bool complete_submission = !intent.submitted_task_id.empty()
        && intent.submitted_thread_revision && intent.committed_at_ms;
    if (any_submission != complete_submission) return false;
    if (complete_submission
        && (!v3_task_id(intent.submitted_task_id)
            || *intent.submitted_thread_revision
                != intent.expected_thread_revision + 1
            || *intent.committed_at_ms < intent.created_at_ms)) {
        return false;
    }

    using State = LocalGooseDialogueResponderIntentState;
    if (intent.state == State::prepared) {
        return !complete_submission && !intent.completed_at_ms
            && intent.terminal_reason.empty();
    }
    if (intent.state == State::submitted) {
        return complete_submission && !intent.completed_at_ms
            && intent.terminal_reason.empty();
    }
    if (intent.state == State::completed) {
        return complete_submission && intent.completed_at_ms
            && *intent.completed_at_ms >= *intent.committed_at_ms
            && intent.terminal_reason.empty();
    }
    if (!bounded_nonempty(intent.terminal_reason, max_reason_bytes)
        || intent.completed_at_ms) {
        return false;
    }
    if (intent.state == State::manual_review) return true;
    return !complete_submission;
}

} // namespace gaudere_agent
