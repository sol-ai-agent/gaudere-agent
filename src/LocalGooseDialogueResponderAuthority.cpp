#include "LocalGooseDialogueResponderAuthority.hpp"

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
constexpr const char* intent_prefix =
    "cognition.local-goose-dialogue.responder-intent.v1:";
constexpr std::size_t max_identifier_bytes = 128;
constexpr std::size_t max_purpose_bytes = 1024;
constexpr std::size_t max_message_bytes = 4096;
constexpr std::size_t max_reason_bytes = 1024;
constexpr std::int64_t max_duration_ms = 24LL * 60 * 60 * 1000;
constexpr std::uint64_t max_system_turns = 8;

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

bool bounded_nonempty(const std::string& value, const std::size_t limit) noexcept
{
    return !value.empty() && value.size() <= limit;
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

std::string grant_digest(const LocalGooseDialogueResponderGrant& grant)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-grant.v1|";
    append(canonical, grant.request_id);
    append(canonical, grant.authority_source);
    append(canonical, grant.thread_alias);
    append(canonical, grant.speaker_kind);
    append(canonical, grant.speaker_id);
    append(canonical, static_cast<std::uint64_t>(grant.allowed_message_kinds));
    append(canonical, grant.purpose);
    append(canonical, grant.max_system_turns);
    append(canonical, static_cast<std::uint64_t>(grant.max_lease_ttl_ms));
    append(canonical, static_cast<std::uint64_t>(grant.min_interval_ms));
    append(canonical, static_cast<std::uint64_t>(
        grant.expires_at_ms - grant.created_at_ms));
    return sha256_hex(canonical);
}

std::string decision_digest(const LocalGooseDialogueResponderDecision& decision)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-decision.v1|";
    append(canonical, decision.request_id);
    append(canonical, decision.authority_source);
    append(canonical, decision.grant_id);
    append(canonical, decision.lease_id);
    append(canonical, decision.consumer_id);
    append(canonical, decision.completion_sequence);
    append(canonical, decision.event_id);
    append(canonical, decision.result_sha256);
    append(canonical, decision.thread_alias);
    append(canonical, decision.thread_revision);
    append(canonical, decision.task_id);
    append(canonical, decision.observed_speaker_kind);
    append(canonical, decision.observed_speaker_id);
    append(canonical, decision.expected_head_revision);
    append(canonical, decision.expected_head_task_id);
    append(canonical, decision.responder_speaker_kind);
    append(canonical, decision.responder_speaker_id);
    append(canonical, decision.message_kind);
    append(canonical, decision.message);
    append(canonical, static_cast<std::uint64_t>(decision.intent_ttl_ms));
    return sha256_hex(canonical);
}

bool valid_grant_state(const LocalGooseDialogueResponderGrantState state) noexcept
{
    using State = LocalGooseDialogueResponderGrantState;
    return state == State::prepared || state == State::activated
        || state == State::revoked || state == State::expired
        || state == State::manual_review;
}

bool valid_decision_state(
    const LocalGooseDialogueResponderDecisionState state) noexcept
{
    using State = LocalGooseDialogueResponderDecisionState;
    return state == State::prepared || state == State::intent_bound
        || state == State::completed || state == State::conflict
        || state == State::expired || state == State::manual_review;
}

} // namespace

LocalGooseDialogueResponderGrant make_local_goose_dialogue_responder_grant(
    const std::string& request_id,
    const std::string& authority_source,
    const std::string& thread_alias,
    const std::string& speaker_id,
    const std::uint32_t allowed_message_kinds,
    const std::string& purpose,
    const std::uint64_t max_turns,
    const std::int64_t max_lease_ttl_ms,
    const std::int64_t min_interval_ms,
    const std::int64_t created_at_ms,
    const std::int64_t grant_ttl_ms)
{
    LocalGooseDialogueResponderGrant grant;
    grant.request_id = request_id;
    grant.authority_source = authority_source;
    grant.thread_alias = thread_alias;
    grant.speaker_id = speaker_id;
    grant.allowed_message_kinds = allowed_message_kinds;
    grant.purpose = purpose;
    grant.max_system_turns = max_turns;
    grant.max_lease_ttl_ms = max_lease_ttl_ms;
    grant.min_interval_ms = min_interval_ms;
    grant.created_at_ms = created_at_ms;
    if (created_at_ms >= 0 && grant_ttl_ms > 0
        && created_at_ms <= std::numeric_limits<std::int64_t>::max() - grant_ttl_ms) {
        grant.expires_at_ms = created_at_ms + grant_ttl_ms;
    }
    if (grant.expires_at_ms > grant.created_at_ms) {
        grant.grant_id =
            std::string{local_goose_dialogue_responder_grant_prefix}
            + grant_digest(grant);
    }
    return grant;
}

bool valid_local_goose_dialogue_responder_grant(
    const LocalGooseDialogueResponderGrant& grant) noexcept
{
    if (!prefixed_sha256(grant.grant_id,
                         local_goose_dialogue_responder_grant_prefix)
        || !safe_identifier(grant.request_id)
        || !safe_identifier(grant.authority_source)
        || !safe_identifier(grant.thread_alias)
        || grant.speaker_kind != "system"
        || !safe_identifier(grant.speaker_id)
        || grant.allowed_message_kinds == 0
        || (grant.allowed_message_kinds
            & ~local_goose_dialogue_responder_message_all) != 0
        || !bounded_nonempty(grant.purpose, max_purpose_bytes)
        || grant.max_system_turns == 0
        || grant.max_system_turns > max_system_turns
        || grant.max_lease_ttl_ms <= 0
        || grant.max_lease_ttl_ms > max_duration_ms
        || grant.min_interval_ms < 0
        || grant.min_interval_ms > max_duration_ms
        || grant.created_at_ms < 0
        || grant.expires_at_ms <= grant.created_at_ms
        || grant.expires_at_ms - grant.created_at_ms > max_duration_ms
        || !valid_grant_state(grant.state)
        || grant.terminal_reason.size() > max_reason_bytes) {
        return false;
    }
    if (grant.grant_id
        != std::string{local_goose_dialogue_responder_grant_prefix}
            + grant_digest(grant)) {
        return false;
    }

    const bool activated = grant.activated_lease_id.has_value()
        || grant.activated_at_ms.has_value();
    if (grant.activated_lease_id.has_value() != grant.activated_at_ms.has_value()) {
        return false;
    }
    if (activated
        && (!safe_identifier(*grant.activated_lease_id)
            || *grant.activated_at_ms < grant.created_at_ms
            || *grant.activated_at_ms >= grant.expires_at_ms)) {
        return false;
    }

    using State = LocalGooseDialogueResponderGrantState;
    if (grant.state == State::prepared) {
        return !activated && grant.terminal_reason.empty() && !grant.terminal_at_ms;
    }
    if (grant.state == State::activated) {
        return activated && grant.terminal_reason.empty() && !grant.terminal_at_ms;
    }
    if (activated || grant.terminal_reason.empty() || !grant.terminal_at_ms
        || *grant.terminal_at_ms < grant.created_at_ms) {
        return false;
    }
    if (grant.state == State::expired && *grant.terminal_at_ms < grant.expires_at_ms) {
        return false;
    }
    return true;
}

LocalGooseDialogueResponderDecision
make_local_goose_dialogue_responder_decision(
    const std::string& request_id,
    const std::string& authority_source,
    const std::string& grant_id,
    const std::string& lease_id,
    const std::string& consumer_id,
    const std::uint64_t completion_sequence,
    const std::string& event_id,
    const std::string& result_sha256,
    const std::string& thread_alias,
    const std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& observed_speaker_kind,
    const std::string& observed_speaker_id,
    const std::uint64_t expected_head_revision,
    const std::string& expected_head_task_id,
    const std::string& responder_speaker_id,
    const std::string& message_kind,
    const std::string& message,
    const std::int64_t intent_ttl_ms,
    const std::int64_t created_at_ms)
{
    LocalGooseDialogueResponderDecision decision;
    decision.request_id = request_id;
    decision.authority_source = authority_source;
    decision.grant_id = grant_id;
    decision.lease_id = lease_id;
    decision.consumer_id = consumer_id;
    decision.completion_sequence = completion_sequence;
    decision.event_id = event_id;
    decision.result_sha256 = result_sha256;
    decision.thread_alias = thread_alias;
    decision.thread_revision = thread_revision;
    decision.task_id = task_id;
    decision.observed_speaker_kind = observed_speaker_kind;
    decision.observed_speaker_id = observed_speaker_id;
    decision.expected_head_revision = expected_head_revision;
    decision.expected_head_task_id = expected_head_task_id;
    decision.responder_speaker_id = responder_speaker_id;
    decision.message_kind = message_kind;
    decision.message = message;
    decision.intent_ttl_ms = intent_ttl_ms;
    decision.created_at_ms = created_at_ms;
    decision.decision_id =
        std::string{local_goose_dialogue_responder_decision_prefix}
        + decision_digest(decision);
    return decision;
}

bool valid_local_goose_dialogue_responder_decision(
    const LocalGooseDialogueResponderDecision& decision) noexcept
{
    if (!prefixed_sha256(decision.decision_id,
                         local_goose_dialogue_responder_decision_prefix)
        || !safe_identifier(decision.request_id)
        || !safe_identifier(decision.authority_source)
        || !prefixed_sha256(decision.grant_id,
                            local_goose_dialogue_responder_grant_prefix)
        || !safe_identifier(decision.lease_id)
        || !safe_identifier(decision.consumer_id)
        || decision.completion_sequence == 0
        || decision.completion_sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(decision.event_id, completion_event_prefix)
        || !lowercase_sha256(decision.result_sha256)
        || !safe_identifier(decision.thread_alias)
        || decision.thread_revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(decision.task_id, v3_task_prefix)
        || decision.observed_speaker_kind != "human"
        || !safe_identifier(decision.observed_speaker_id)
        || decision.expected_head_revision != decision.thread_revision
        || decision.expected_head_task_id != decision.task_id
        || decision.responder_speaker_kind != "system"
        || !safe_identifier(decision.responder_speaker_id)
        || local_goose_dialogue_responder_message_kind_bit(decision.message_kind) == 0
        || !bounded_nonempty(decision.message, max_message_bytes)
        || decision.intent_ttl_ms <= 0
        || decision.intent_ttl_ms > max_duration_ms
        || decision.created_at_ms < 0
        || !valid_decision_state(decision.state)
        || decision.terminal_reason.size() > max_reason_bytes) {
        return false;
    }
    if (decision.decision_id
        != std::string{local_goose_dialogue_responder_decision_prefix}
            + decision_digest(decision)) {
        return false;
    }

    const bool bound = decision.intent_id.has_value()
        || decision.bound_at_ms.has_value();
    if (decision.intent_id.has_value() != decision.bound_at_ms.has_value()) {
        return false;
    }
    if (bound
        && (!prefixed_sha256(*decision.intent_id, intent_prefix)
            || *decision.bound_at_ms < decision.created_at_ms)) {
        return false;
    }

    using State = LocalGooseDialogueResponderDecisionState;
    if (decision.state == State::prepared) {
        return !bound && !decision.completed_at_ms
            && decision.terminal_reason.empty() && !decision.terminal_at_ms;
    }
    if (decision.state == State::intent_bound) {
        return bound && !decision.completed_at_ms
            && decision.terminal_reason.empty() && !decision.terminal_at_ms;
    }
    if (decision.state == State::completed) {
        return bound && decision.completed_at_ms
            && *decision.completed_at_ms >= *decision.bound_at_ms
            && decision.terminal_reason.empty() && !decision.terminal_at_ms;
    }
    if (decision.completed_at_ms || decision.terminal_reason.empty()
        || !decision.terminal_at_ms
        || *decision.terminal_at_ms < decision.created_at_ms) {
        return false;
    }
    return true;
}

} // namespace gaudere_agent
