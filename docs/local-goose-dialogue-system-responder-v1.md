# Local Goose dialogue system responder v1 — bounded intervention contract

Status: Stage 9J1 design contract only.

This design follows the successful production proof of the durable dialogue completion feed: consumer `sol` acknowledged exactly sequence 1; sequence 2 remains pending; provider total remains 10; `Network=none`; no Task, dialogue turn, stimulus, or provider call was created by that proof.

This document authorizes no production effect, no provider call, no Local Goose cycle stimulus, no completion ACK, and no system-authored dialogue turn.

## Core rule

The responder must never implement:

`completion event exists -> automatically submit a successor`

A completion event is evidence only. It is not authority to speak.

Responder v1 separates three independent stages:

1. observation of one canonical completion event;
2. creation of a durable intervention intent under a finite responder lease;
3. dispatch of exactly that intent through the existing preferred-head V3 CAS path.

Observation does not create an intent. ACK does not create an intent. Intent creation does not create a Task. A completed responder turn does not create or renew another intent or lease.

## Durable responder sidecar

Use a separate owner-only SQLite sidecar, initially schema version 1, e.g. `local-goose-dialogue-responder.db`.

It stores finite leases and immutable intervention intents plus dispatch outcomes. It does not replace the completion-feed cursor or the preferred-thread store.

## Finite responder lease

A lease contains at least:

- `lease_id`;
- `thread_alias`;
- `speaker_kind=system`;
- admitted `speaker_id`;
- allowed message-kind set;
- bounded explicit `purpose`;
- `max_system_turns`;
- `turns_committed`;
- `issued_at_ms`;
- `expires_at_ms`;
- `min_interval_ms`;
- `next_eligible_at_ms`;
- state and terminal reason.

Initial states: `active`, `exhausted`, `expired`, `revoked`, `manual_review`.

Only one active lease may exist for one preferred thread. Initial hard bounds: `1 <= max_system_turns <= 8`, expiry required and at most 24 h after issue, bounded purpose, admitted system speaker, finite minimum interval.

No lease auto-renewal is allowed.

## Durable intervention intent

An intent is the sole durable cause that may authorize one system-authored successor. It binds exactly:

- `intent_id`;
- `lease_id`;
- `thread_alias`;
- completion `sequence`;
- completion `event_id`;
- completion `result_sha256`;
- triggering `task_id`;
- expected preferred `thread_revision`;
- expected preferred `head_task_id`;
- admitted `speaker_kind=system`;
- admitted `speaker_id`;
- admitted `message_kind`;
- bounded exact `message`;
- deterministic V3 `request_id`;
- creation and expiry times.

The message is already decided by the policy/admission layer. The initial dispatcher does not ask another model to invent content.

Intent identity is deterministic over canonical evidence including lease ID, trigger event ID, expected revision, actor, message kind and exact message bytes. The V3 request ID is deterministically derived from the intent ID.

Retrying the same intent therefore reaches the existing V3 request-id idempotency path; different content under the same identity is a conflict.

## Intent admission

Intent creation is allowed only when:

- the lease exists, is active and unexpired;
- quota remains and minimum interval has elapsed;
- speaker/message kind are admitted;
- the exact completion event exists at the supplied sequence;
- event ID and result SHA match exactly;
- event belongs to the lease thread;
- preferred thread exists and exactly matches expected revision/head Task ID;
- the trigger event corresponds to that preferred head;
- no committed intent already consumes the same trigger under the lease.

Intent admission does not ACK the event and does not create a Task.

## Dispatch state machine

Intent dispatch states: `prepared`, `submitted`, `completed`, `conflict`, `expired`, `manual_review`.

Before submission, the dispatcher revalidates the lease, quota, interval, trigger identity/hash, preferred head and intent expiry. Any stale evidence fails closed without creating a Task.

A valid intent submits through the existing `local-thread-v3-send` semantics using the intent's deterministic request ID, exact expected revision, admitted provenance and exact stored message. It never bypasses preferred-head CAS.

The existing retry behavior is relied upon: if the head advanced by exactly one to the same canonical request, retry returns the already-submitted Task; any other head/request is conflict.

A lease turn is counted only once the intent is durably reconciled with the canonical submitted V3 Task and the preferred head has advanced to it. Replaying that same intent never consumes another turn.

## Completion ACK ordering

For an intent-driven intervention, ordering is:

1. read and validate trigger event;
2. prepare durable intent;
3. submit/reconcile the exact V3 successor;
4. durably commit responder accounting;
5. only then ACK the trigger sequence.

Failure before canonical reconciliation leaves the event redeliverable. Crash after successful submission but before ACK is recovered using deterministic intent/request identity. ACK retry remains idempotent.

## Consumer identity

Automatic responder consumption must not silently reuse an unrelated consumer identity. The production responder consumer identity must be fixed and documented before activation.

If the existing manual `sol` cursor is migrated, that migration is a separate explicit state transition. A fresh responder consumer must not silently replay historical events. Stage 9J1 does not choose or perform that migration.

## Recovery and concurrency

On restart:

- expire leases before dispatch;
- revalidate prepared intents from durable evidence;
- reconcile submitted intents by deterministic request identity and preferred-head state;
- never resubmit a canonical turn as a different Task;
- ambiguous evidence becomes `manual_review`;
- never ACK before canonical dispatch reconciliation succeeds;
- never create an intent merely because an event is pending.

Concurrency invariants:

- one active lease per thread;
- one committed intervention per lease + trigger event;
- preferred-head CAS remains final serialization;
- a human/system race may make an intent stale;
- stale intents never rebase automatically;
- no automatic branch is created on conflict.

## Authority boundaries

Responder v1 must not gain authority to enable/call OpenAI, consume provider budget, mutate the autonomous cycle, create stimuli, schedule/accept wakes, execute general host commands, bypass preferred-head CAS, or mutate feed history.

The safe default for any missing or ambiguous evidence is no action.

## Production defaults

Even when responder code is present in production:

- no lease exists by default;
- no worker is armed by default;
- no pending completion is consumed automatically;
- no system-authored successor is emitted automatically;
- provider remains OFF;
- Stage 9I3 sequence 2 remains pending until a later explicit gate.

Code presence is not activation.

## Delivery stages

1. **9J1 — design contract**: this document.
2. **9J2 — canonical store contract**: lease/intent schemas, identities, state transitions, unit/adversarial tests; no production wiring.
3. **9J3 — bounded dispatcher**: deterministic preferred-head submission/recovery plus typed control surfaces, idle by default.
4. **9J4 — isolated Fedora proof**: finite lease, one intervention, ACK ordering, restart/retry/stale/expiry/exhaustion, zero provider/cycle effects.
5. **9J5 — production code deployment**: image/code only; no active lease and no automatic consumption.
6. **9J6 — first production responder activation**: separate explicit human authorization with a very small finite lease and exact postconditions.

Every stage preserves:

`code -> CI green -> merge -> Fedora deployment/integration -> real Fedora proof`

No design, merge or code deployment authorizes a production lease, completion ACK, system-authored turn, provider call or autonomous-cycle effect.
