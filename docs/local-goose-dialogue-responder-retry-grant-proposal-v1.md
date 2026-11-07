# Generation-1 responder retry — inert grant preparation contract v1

Status: **source-only, disabled, not deployed, not an authority**.

This is the next core Gaudere tranche after PR #275, which introduced a separate
append-only SQLite **evidence scope**. The real operator-supplied Fedora
frozen-copy proof in #274 returned
`FEDORA_GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_ISOLATED=PASS` and
`ELIGIBILITY=evidence_consistent_no_authority`. That verified consistency,
not permission to replay or answer the held human sequence 4.

## Why a distinct contract

The historical responder v3 sidecar already records one decision per
`(consumer_id,event_id)` and one historical disposition for the same event.
It cannot be reopened or rewritten to manufacture a second attempt. A retry
must use new, generation-aware identities and independent accounting.

`LocalGooseDialogueResponderRetryGrantProposal` is a **canonical immutable
preparation envelope only**, deliberately not the historical grant type.
No production service constructs it, stores it or activates it.

It binds all of:

- `scope_id` and admitted `lineage_id`, with the original exact
  `consumer_id`, event/result/task ID, thread `main`, sequence 4,
  expected head revision 3, original human speaker and retry generation 1.
- One independent `request_id`, a fixed retry-only
  `authority_source=local-typed-operator-retry-v1` and an
  `approval_reference`. The latter is an **unverified caller-supplied
  identifier**, not evidence of identity, consent or an approved activation.
- One system responder actor, **intervention** as the only permissible
  message-kind bit, `max_system_turns=1`, explicit human-readable purpose,
  and bounded preparation timestamps.

The canonical proposal ID hashes the complete immutable envelope, including
reference, scope, source, speaker, purpose, budgets, TTL and state. The
`matches_local_goose_dialogue_responder_retry_grant_scope` helper verifies
every copied scope field and rejects a stale/bogus scope ID. The proposal type
contains only `prepared_only` as a valid state.

Limits: grant proposal TTL at most **15 minutes**, potential future lease TTL
at most **2 minutes** and never exceeding grant TTL. Invalid/overflowed clocks,
unknown source, wrong event, generation 2, ACK drift, changed actor, unknown
message classes, additional system turns, mutated digest or non-ASCII/control
purpose fail closed.

## Critical boundary

A syntactically valid proposal **is not approval**. The model may be built
from an arbitrary fake `approval_reference`; only a later *trusted typed
operator boundary* can authenticate explicit human approval against the
requested exact lineage, speaker, message and budget. That future check must
also re-open the actual authoritative stores and prove generation-1 eligibility
fresh, not rely on these purely in-memory invariants or an old PASS marker.
Expired envelopes require a new human gate, not silent renewal.

This tranche provides **no SQLite writer, production path, service, message,
grant activation, lease, decision, intent, disposition, Task submission, feed
ACK, provider call, network access, B10 operation or Fedora host action**.
In particular, it does not modify historical responder v3 tables or the retry
audit or scope sidecars. It is excluded from `main.cpp`, `Makefile.am` and
production `Containerfile`.

## Evidence and next independent gates

The new workflow independently compiles a synthetic Fedora 44 standalone test
under `-std=c++17 -Wall -Wextra -Wpedantic -Werror`. It covers deterministic
identities, exact scope matching, conflicting event/generation/source and
approval reference, actor/message constraints, duration/overflow boundaries,
payload tampering, invalid scopes and prepared-only state. It runs no Fedora
host command.

After code review and CI, separate future tranches are required to:

1. Design and validate a trusted operator approval source, including exact
   human identity and no spoofing by self-asserted IDs; persist immutable
   grant preparation in a dedicated, versioned store.
2. Add **separate** explicit human activation with durable finite lease,
   generation-aware uniqueness, exact time and historical-state revalidation.
3. Add retry-specific decision/intent/disposition, resource accounting and
   approved dispatcher integration without any historical identity collision.
4. Run crash/restart and isolated-copy tests and then a **new explicit Fedora
   operator gate** before any production install/activation/dispatch.

Current production evidence remains unchanged: original consumer cursor 3,
next human completion 4 unACKed, head revision 3, no retry authority or Task,
provider OFF, `Network=none`, provider total 10.
