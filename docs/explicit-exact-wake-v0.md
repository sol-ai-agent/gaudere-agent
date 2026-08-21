# Explicit exact wake v0

## Decision

The next slice is a provider-free proof of one durable exact wake. An operator may
explicitly accept one already-normalized `propose_wake` result from a succeeded
`cognition.reflect.v1` task. Acceptance persists one immutable deadline. When that
deadline becomes due, the main worker records a durable `fired` state.

This slice does not create a successor Task, submit a reflection, invoke a provider,
or perform any external effect. A fired wake is observable state only. It is not
authority to continue a cognition loop.

The first permanent reflection, `production-reflection-first`, returned `stop`.
It has no wake proposal and is therefore permanently ineligible for acceptance.
Design, implementation, tests, migration preparation, and deployment do not
authorize a third production provider call or the fabrication of a replacement
proposal.

## Why acceptance is explicit

`cognition.reflect.v1` deliberately persists a proposal rather than a schedule. The
new boundary keeps the authority transition separate and observable:

1. a terminal Task contains a strictly normalized decision;
2. a human explicitly names that Task through local live control;
3. the sole worker validates the immutable result and persists the wake;
4. the scheduler waits for the persisted deadline without polling;
5. the sole worker records the wake as fired when it is due.

No model output is interpreted directly by the scheduler. Free text, a pending or
failed Task, a `stop` decision, an unknown schema, and an out-of-range delay are all
rejected before any wake record is created.

## Durable core primitive

The reusable Gaudere library should add a provider-agnostic `WakeIntent`, store, and
runtime. The SQLite implementation introduces additive schema v4. A wake intent
contains only bounded, auditable data:

- a deterministic identity equal to the immutable source Task identity in the
  separate wake-intent namespace;
- the source identity;
- the acceptance time and exact UTC deadline, stored as integer milliseconds;
- one of `scheduled`, `fired`, `revoked`, or `manual_review`;
- an optional terminal time and bounded terminal reason.

Wake rows are immutable except for the single transition from `scheduled` to one
terminal state. `manual_review` is a fail-closed terminal state for detected clock
or durable-invariant uncertainty; it never fires automatically. Rows are never
deleted or recycled. The store exposes the earliest scheduled deadline and performs
acceptance and terminal transitions atomically. SQLite constraints reject an
invalid status/timestamp combination.

The generic library does not understand cognition content. Agent code validates the
source Task and supplies the already-derived deadline to the core runtime. The
source Task remains the canonical audit record for the normalized decision and
provider usage, so raw provider JSON and duplicated model output are not added to
the wake table.

## Hard v0 bound and idempotency

V0 permits at most **one accepted wake for the lifetime of a database**. Revoking
that wake does not restore the slot. This intentionally hard bootstrap bound is
enforced in the same SQLite transaction that inserts the first wake, not by a
process-local counter.

The source identity is unique. Repeating acceptance for the same source returns the
existing record without changing its deadline. A different source is rejected once
the lifetime slot has been consumed. Concurrent requests are serialized by the sole
worker and still meet the database constraint.

This is a wake budget, not the provider budget. Acceptance and firing consume no
provider permit.

## Agent acceptance contract

The only creation path is a bounded AF_UNIX live-control operation equivalent to:

```text
accept-wake SOURCE_TASK_ID
```

It accepts no arbitrary timestamp or delay. On the main worker, the command loads
the source Task and requires all of the following:

- kind `cognition.reflect.v1`;
- terminal status `succeeded`;
- result content type
  `application/vnd.gaudere.cognition-decision+json`;
- schema `gaudere.cognition.decision.v1`;
- decision `propose_wake`;
- the existing strict canonical object with an integer `wake_after_seconds` from
  900 through 86400 and no unknown fields.

The worker samples its clock once. `accepted_at` is that sample and `due_at` is
`accepted_at + wake_after_seconds`, with checked arithmetic and millisecond
precision. The durable transaction completes before the in-memory scheduler is
notified. The command reports the complete stored identity, source, status, and
timestamps.

No offline command may bypass the live owner while the service owns the database.
No command accepts raw decision JSON. Offline tests may construct fixture records in
a disposable database through test-only code; they do not need a provider call.

## Firing and scheduler semantics

The scheduler continues to own no thread. At startup and after every worker event,
the controller requests the minimum of:

- the next active Task lease-recovery deadline;
- the earliest scheduled wake-intent deadline.

The existing scheduler keeps the earliest requested deadline and blocks with
`wait_until`; there is no periodic tick. On a scheduler wake, the main worker first
transitions every due wake intent, then performs normal lease recovery and Task
dispatch, and finally re-arms the next durable deadline.

`due_at` is a lower bound, not a hard real-time guarantee. If the sole worker is
finishing bounded work, the process is stopped, or the host is down at that instant,
`fired_at` is the first safe worker observation at or after `due_at`. Both timestamps
remain observable, so lateness is measurable. A forward wall-clock jump may make a
wake immediately overdue; it is then fired once. If a worker clock sample is before
the persisted `accepted_at`, the worker atomically moves the intent to
`manual_review` instead of arming or firing it.

Revocation can leave an already-armed in-memory deadline in the current Scheduler.
That deadline may cause one inert process wake, after which durable state is
recomputed. It cannot transition the revoked record, create work, or repeat. This is
an event-driven stale notification, not polling. A fresh process never arms a
revoked record.

## Revocation and human recovery

A second bounded live-control operation provides permanent revocation:

```text
revoke-wake WAKE_ID REASON
```

The reason is non-empty and bounded. The main worker accepts revocation only while
the record is `scheduled` and its clock sample is strictly before `due_at`. At or
after the deadline, firing wins even if a revocation request was queued earlier but
not yet processed. This makes restart and race behavior independent of thread
timing.

Inspection is always available through the live owner. Recovery tooling must be
able to report the one record and its source, deadline, terminal state, terminal
time, and reason. Neither restart nor reinstallation clears the lifetime bound.
A `manual_review` wake has no automatic resume transition in v0. Human rollback
remains a stopped-service restoration of a verified pre-v4 backup; any later
reconciliation command requires its own design and proof.

## Crash and restart reconciliation

Every material boundary has one deterministic outcome:

| Crash point | Durable state after restart | Recovery |
| --- | --- | --- |
| Before the acceptance transaction commits | No wake | Nothing is armed; the operator may retry the same command. |
| After commit, before the in-memory request | `scheduled` | Startup loads and arms the exact stored deadline. |
| At/after the deadline, before `fired` commits | `scheduled` and overdue | Startup or the next worker event commits `fired` once. |
| After `fired` commits | `fired` | Terminal state is observed; no repeat occurs. |
| After revocation commits | `revoked` | Terminal state is observed; no firing occurs. |
| After detected clock rollback commits | `manual_review` | Terminal state is reported; no automatic firing or resume occurs. |

Firing has no external effect, so reevaluating an overdue `scheduled` row before its
terminal transaction commits is safe. Future slices must not attach an external
effect to this transition without a separate durable effect marker and ambiguity
policy.

## Thread and ownership model

The control thread may validate protocol bounds, enqueue a command, and wake the
scheduler. It never reads or writes Runtime or SQLite. Signal handling remains
atomics plus wake/stop only. Acceptance, rejection, revocation, overdue recovery,
firing, inspection reads used to build replies, and Task transitions all run on the
sole main worker.

The existing `state.db.lock` remains mandatory for service, offline inspection,
migration, backup, and restore. No TCP listener, timer thread, polling loop, shell,
new provider, or general network capability is introduced.

## Delivery gates

This capability is delivered in deliberately separate gates:

1. **Core implementation:** `WakeIntent`, runtime, SQLite schema v4, fake-clock unit
   tests, idempotency, lifetime bound, and reopen/recovery tests.
2. **Agent integration:** strict source validation, accept/revoke/inspect live
   control, shared exact scheduler integration, and disposable end-to-end tests.
3. **Migration proof:** stopped-service backup, disposable v3 to v4 migration,
   restored-copy validation, and documented rollback.
4. **Production decision:** a later explicit human authorization, with an eligible
   durable `propose_wake` source, before any real wake is accepted.

The first three gates perform no provider call. Merging implementation does not
authorize deployment. Deploying schema v4 does not authorize accepting a wake. The
current production `stop` result cannot satisfy the fourth gate.

## Required deterministic tests

Before any production deployment, tests must prove:

- every accepted and rejected source shape;
- checked deadline arithmetic and both delay bounds;
- one lifetime acceptance, duplicate idempotency, and no slot refund on revoke;
- exact earliest-deadline selection alongside Task lease recovery;
- no periodic wake while no deadline or command exists;
- acceptance commit followed by simulated crash and reopen;
- overdue firing after reopen, exactly once;
- revocation before due and rejection at/after due;
- deterministic ordering when accept, revoke, shutdown, and due events meet;
- clock-forward behavior and fail-closed detected rollback;
- clean shutdown with a future scheduled wake and exact re-arm on restart;
- zero successor Tasks, zero provider invocations, and unchanged provider budget;
- v3 to v4 migration, v4 reopen, backup validation, and human rollback procedure.
