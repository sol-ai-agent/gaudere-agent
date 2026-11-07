#include "LocalGooseDialogueResponderRetryGrantProposal.hpp"

#include "LocalGooseDialogueResponder.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <string>

namespace {

using namespace gaudere_agent;
using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Scope = LocalGooseDialogueResponderRetryScope;
using State = LocalGooseDialogueResponderRetryGrantProposalState;

std::string hash_id(const std::string& prefix, const char c)
{
    return prefix + std::string(64, c);
}

LocalGooseDialogueResponderRetryLineage lineage(const char event_hex = 'a')
{
    const auto l = make_local_goose_dialogue_responder_retry_lineage(
        "lineage-request-1", "system-responder-v1", "main", 4,
        hash_id("cognition.local-goose-dialogue.completion.v1:", event_hex),
        std::string(64, 'b'), 3,
        hash_id("cognition.local-goose-dialogue.v3:", 'c'),
        "bertrand",
        hash_id("cognition.local-goose-dialogue.responder-grant.v1:", 'd'),
        "historical-lease-1",
        hash_id("cognition.local-goose-dialogue.responder-decision.v1:", 'e'),
        hash_id("cognition.local-goose-dialogue.responder-intent.v1:", 'f'),
        hash_id("cognition.local-goose-dialogue.responder-disposition.v1:", '1'),
        "", "lease_expired", "bound_intent_expired_before_dispatch",
        "lease_or_intent_expired", "lease_or_intent_expired", 1000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(l));
    return l;
}

Scope scope(const char event_hex = 'a')
{
    const auto s = make_local_goose_dialogue_responder_retry_scope(
        "scope-request-1", lineage(event_hex), 2000);
    assert(s);
    return *s;
}

Proposal proposal(const Scope& s)
{
    const auto p = make_local_goose_dialogue_responder_retry_grant_proposal(
        s, "grant-proposal-request-1", "operator-approval-reference-1",
        "Single explicit generation-one reply preparation", 3000, 300000, 120000);
    assert(p);
    assert(valid_local_goose_dialogue_responder_retry_grant_proposal(*p));
    assert(matches_local_goose_dialogue_responder_retry_grant_scope(*p, s));
    return *p;
}

void test_canonical_deterministic_and_bounded()
{
    const auto s = scope();
    const auto p = proposal(s);
    assert(p.state == State::prepared_only);
    assert(p.authority_source == "local-typed-operator-retry-v1");
    assert(p.max_system_turns == 1);
    assert(p.retry_generation == 1);
    assert(p.completion_sequence == 4);
    assert(p.expected_head_revision == 3);
    assert(p.allowed_message_kinds
        == local_goose_dialogue_responder_message_intervention);
    assert(p.expires_at_ms == 303000);
    assert(p.max_lease_ttl_ms == 120000);
    assert(proposal(s).proposal_id == p.proposal_id);
    assert(!matches_local_goose_dialogue_responder_retry_grant_scope(
        p, scope('2')));

    const auto distinct = make_local_goose_dialogue_responder_retry_grant_proposal(
        s, "grant-proposal-request-2", "operator-approval-reference-1",
        "Single explicit generation-one reply preparation", 3000, 300000, 120000);
    assert(distinct && distinct->proposal_id != p.proposal_id);
}

void test_tamper_rejected()
{
    const auto s = scope();
    const auto base = proposal(s);
    auto p = base;
    p.state = static_cast<State>(1);
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.authority_source = "local-typed-operator-v1";
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.approval_reference = "operator-approval-other";
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.scope_id = hash_id(
        "cognition.local-goose-dialogue.responder-retry-scope.v1:", 'f');
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.retry_generation = 2;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.completion_sequence = 5;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.expected_head_revision = 4;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.max_system_turns = 2;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.allowed_message_kinds = local_goose_dialogue_responder_message_all;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.responder_speaker_id = "someone-else";
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.purpose = "Different purpose";
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.expires_at_ms = p.prepared_at_ms - 1;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.max_lease_ttl_ms = 120001;
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
    p = base;
    p.proposal_id = hash_id(
        "cognition.local-goose-dialogue.responder-retry-grant-proposal.v1:", 'a');
    assert(!valid_local_goose_dialogue_responder_retry_grant_proposal(p));
}

void test_invalid_input_fails_closed()
{
    const auto s = scope();
    const auto make = [&](const std::string& request,
                          const std::string& approval,
                          const std::string& purpose,
                          std::int64_t when,
                          std::int64_t ttl,
                          std::int64_t lease_ttl) {
        return make_local_goose_dialogue_responder_retry_grant_proposal(
            s, request, approval, purpose, when, ttl, lease_ttl);
    };
    assert(!make("", "approval-1", "purpose", 3000, 1000, 1000));
    assert(!make("request-1", "", "purpose", 3000, 1000, 1000));
    assert(!make("request-1", "approval with spaces",
        "purpose", 3000, 1000, 1000));
    assert(!make("request-1", "approval-1", "", 3000, 1000, 1000));
    assert(!make("request-1", "approval-1",
        "bad\npurpose", 3000, 1000, 1000));
    assert(!make("request-1", "approval-1", "purpose", 1999, 1000, 1000));
    assert(!make("request-1", "approval-1", "purpose", 3000, 0, 1));
    assert(!make("request-1", "approval-1", "purpose", 3000, 900001, 1));
    assert(!make("request-1", "approval-1", "purpose", 3000, 1000, 0));
    assert(!make("request-1", "approval-1", "purpose", 3000, 1000, 1001));
    assert(!make("request-1", "approval-1", "purpose", 3000, 200000, 120001));
    assert(!make("request-1", "approval-1", "purpose", -1, 1000, 1000));
    assert(!make("request-1", "approval-1", "purpose",
        std::numeric_limits<std::int64_t>::max() - 9, 10, 1));

    auto bad_scope = s;
    bad_scope.retry_generation = 2;
    assert(!make_local_goose_dialogue_responder_retry_grant_proposal(
        bad_scope, "request-1", "approval-1", "purpose", 3000, 1000, 1));
    bad_scope = s;
    bad_scope.scope_id = hash_id(
        "cognition.local-goose-dialogue.responder-retry-scope.v1:", '0');
    assert(!make_local_goose_dialogue_responder_retry_grant_proposal(
        bad_scope, "request-1", "approval-1", "purpose", 3000, 1000, 1));
}

void test_upper_bound_is_accepted()
{
    const auto s = scope();
    const auto p = make_local_goose_dialogue_responder_retry_grant_proposal(
        s, "request-valid-end", "approval-valid-end", "Bounded one shot",
        2000, 900000, 120000);
    assert(p);
    assert(p->expires_at_ms == 902000);
    assert(valid_local_goose_dialogue_responder_retry_grant_proposal(*p));
    assert(matches_local_goose_dialogue_responder_retry_grant_scope(*p, s));
}

} // namespace

int main()
{
    test_canonical_deterministic_and_bounded();
    test_tamper_rejected();
    test_invalid_input_fails_closed();
    test_upper_bound_is_accepted();
    return 0;
}
