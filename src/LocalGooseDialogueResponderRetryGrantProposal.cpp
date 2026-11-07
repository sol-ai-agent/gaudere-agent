#include "LocalGooseDialogueResponderRetryGrantProposal.hpp"

#include "LocalGooseDialogueResponder.hpp"
#include "Sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace gaudere_agent {
namespace {

using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Scope = LocalGooseDialogueResponderRetryScope;
using State = LocalGooseDialogueResponderRetryGrantProposalState;

bool safe_identifier(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.'
              || c == ':' || c == '-')) return false;
    }
    return true;
}

bool bounded_purpose(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 256) return false;
    for (const unsigned char c : value) {
        if (c < 32 || c > 126) return false;
    }
    return true;
}

bool lower_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& id, const char* prefix) noexcept
{
    const std::string p(prefix);
    return id.size() == p.size() + 64
        && id.compare(0, p.size(), p) == 0
        && lower_sha256(id.substr(p.size()));
}

void append(std::string& canonical, const std::string& value)
{
    canonical += std::to_string(value.size()) + ":" + value + "|";
}
void append(std::string& canonical, const std::uint64_t value)
{
    canonical += std::to_string(value) + "|";
}

std::string proposal_identity(const Proposal& p)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-retry-grant-proposal.v1|";
    append(canonical, p.request_id);
    append(canonical, p.authority_source);
    append(canonical, p.approval_reference);
    append(canonical, p.scope_id);
    append(canonical, p.lineage_id);
    append(canonical, p.consumer_id);
    append(canonical, p.event_id);
    append(canonical, p.result_sha256);
    append(canonical, p.task_id);
    append(canonical, p.thread_alias);
    append(canonical, p.observed_speaker_kind);
    append(canonical, p.observed_speaker_id);
    append(canonical, p.responder_speaker_kind);
    append(canonical, p.responder_speaker_id);
    append(canonical, p.purpose);
    append(canonical, static_cast<std::uint64_t>(p.retry_generation));
    append(canonical, p.completion_sequence);
    append(canonical, p.expected_head_revision);
    append(canonical, static_cast<std::uint64_t>(p.allowed_message_kinds));
    append(canonical, p.max_system_turns);
    append(canonical, static_cast<std::uint64_t>(p.max_lease_ttl_ms));
    append(canonical, static_cast<std::uint64_t>(p.prepared_at_ms));
    append(canonical, static_cast<std::uint64_t>(p.expires_at_ms));
    append(canonical, static_cast<std::uint64_t>(p.state));
    return std::string(local_goose_dialogue_responder_retry_grant_proposal_prefix)
        + sha256_hex(canonical);
}

} // namespace

std::optional<Proposal>
make_local_goose_dialogue_responder_retry_grant_proposal(
    const Scope& scope,
    const std::string& request_id,
    const std::string& approval_reference,
    const std::string& purpose,
    const std::int64_t prepared_at_ms,
    const std::int64_t grant_ttl_ms,
    const std::int64_t max_lease_ttl_ms)
{
    if (!valid_local_goose_dialogue_responder_retry_scope(scope)
        || prepared_at_ms < scope.prepared_at_ms
        || grant_ttl_ms <= 0
        || grant_ttl_ms > local_goose_dialogue_responder_retry_max_grant_ttl_ms
        || prepared_at_ms < 0
        || prepared_at_ms > std::numeric_limits<std::int64_t>::max() - grant_ttl_ms)
        return std::nullopt;

    Proposal p;
    p.request_id = request_id;
    p.approval_reference = approval_reference;
    p.scope_id = scope.scope_id;
    p.lineage_id = scope.lineage_id;
    p.consumer_id = scope.consumer_id;
    p.event_id = scope.event_id;
    p.result_sha256 = scope.result_sha256;
    p.task_id = scope.task_id;
    p.thread_alias = scope.thread_alias;
    p.observed_speaker_kind = scope.observed_speaker_kind;
    p.observed_speaker_id = scope.observed_speaker_id;
    p.purpose = purpose;
    p.retry_generation = scope.retry_generation;
    p.completion_sequence = scope.completion_sequence;
    p.expected_head_revision = scope.expected_head_revision;
    p.allowed_message_kinds = local_goose_dialogue_responder_message_intervention;
    p.max_system_turns = 1;
    p.max_lease_ttl_ms = max_lease_ttl_ms;
    p.prepared_at_ms = prepared_at_ms;
    p.expires_at_ms = prepared_at_ms + grant_ttl_ms;

    // Reject invalid caller input before deriving an immutable identity.
    if (!safe_identifier(p.request_id)
        || !safe_identifier(p.approval_reference)
        || !bounded_purpose(p.purpose)
        || p.max_lease_ttl_ms <= 0
        || p.max_lease_ttl_ms > local_goose_dialogue_responder_retry_max_lease_ttl_ms
        || p.max_lease_ttl_ms > grant_ttl_ms)
        return std::nullopt;

    p.proposal_id = proposal_identity(p);
    if (!valid_local_goose_dialogue_responder_retry_grant_proposal(p)
        || !matches_local_goose_dialogue_responder_retry_grant_scope(p, scope))
        return std::nullopt;
    return p;
}

bool valid_local_goose_dialogue_responder_retry_grant_proposal(
    const Proposal& p) noexcept
{
    if (!prefixed_sha256(
            p.proposal_id,
            local_goose_dialogue_responder_retry_grant_proposal_prefix)
        || !safe_identifier(p.request_id)
        || p.authority_source != local_goose_dialogue_responder_retry_operator_source
        || !safe_identifier(p.approval_reference)
        || !prefixed_sha256(p.scope_id,
            local_goose_dialogue_responder_retry_scope_prefix)
        || !prefixed_sha256(p.lineage_id,
            local_goose_dialogue_responder_retry_lineage_prefix)
        || p.consumer_id != "system-responder-v1"
        || !prefixed_sha256(p.event_id,
            "cognition.local-goose-dialogue.completion.v1:")
        || !lower_sha256(p.result_sha256)
        || !prefixed_sha256(p.task_id,
            "cognition.local-goose-dialogue.v3:")
        || p.thread_alias != "main"
        || p.observed_speaker_kind != "human"
        || p.observed_speaker_id != "bertrand"
        || p.responder_speaker_kind != "system"
        || p.responder_speaker_id != "system-responder-v1"
        || !bounded_purpose(p.purpose)
        || p.retry_generation != 1
        || p.completion_sequence != 4
        || p.expected_head_revision != 3
        || p.allowed_message_kinds
            != local_goose_dialogue_responder_message_intervention
        || p.max_system_turns != 1
        || p.max_lease_ttl_ms <= 0
        || p.max_lease_ttl_ms
            > local_goose_dialogue_responder_retry_max_lease_ttl_ms
        || p.prepared_at_ms < 0
        || p.expires_at_ms <= p.prepared_at_ms
        || p.expires_at_ms - p.prepared_at_ms
            > local_goose_dialogue_responder_retry_max_grant_ttl_ms
        || p.max_lease_ttl_ms > p.expires_at_ms - p.prepared_at_ms
        || p.state != State::prepared_only)
        return false;

    try {
        return p.proposal_id == proposal_identity(p);
    } catch (...) {
        return false;
    }
}

bool matches_local_goose_dialogue_responder_retry_grant_scope(
    const Proposal& p,
    const Scope& scope) noexcept
{
    if (!valid_local_goose_dialogue_responder_retry_grant_proposal(p)
        || !valid_local_goose_dialogue_responder_retry_scope(scope))
        return false;
    return p.scope_id == scope.scope_id
        && p.lineage_id == scope.lineage_id
        && p.consumer_id == scope.consumer_id
        && p.event_id == scope.event_id
        && p.result_sha256 == scope.result_sha256
        && p.task_id == scope.task_id
        && p.thread_alias == scope.thread_alias
        && p.observed_speaker_kind == scope.observed_speaker_kind
        && p.observed_speaker_id == scope.observed_speaker_id
        && p.retry_generation == scope.retry_generation
        && p.completion_sequence == scope.completion_sequence
        && p.expected_head_revision == scope.expected_head_revision
        && p.prepared_at_ms >= scope.prepared_at_ms;
}

} // namespace gaudere_agent
