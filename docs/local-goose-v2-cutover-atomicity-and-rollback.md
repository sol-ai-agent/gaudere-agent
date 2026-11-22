# Local Goose V1 → V2 — atomic cutover and rollback protocol

Status: **source-only design + pure model, NOT a durable implementation or
migration authorization**. No provider, runtime handler, Task submission,
automatic wake, Fedora operation, B10 call, database schema change or
production cursor mutation is contained in this PR.

This is an *internal cognition* continuity gate, not Dev's data collector,
statistics project, portal maintenance or the historical local-activity pulse.

## 1. Current facts and the hard atomicity boundary

- The production V1 cursor is in a dedicated SQLite sidecar.
  `LocalGooseCycleStore::replace()` checks `revision` under one
  `BEGIN IMMEDIATE` transaction, with the current V1 schema version **1**.
  Nothing in the V1 CAS queries a V2 migration fence.
- Work Tasks are in a *different* Core `TaskStore`; creation and
  cursor replacement **cannot currently be one atomic cross-store commit**.
- `LocalGooseCycleService::step()` first returns `dormant` without an
  implicit restart. A generation-2 dormant cursor is the *next opportunity*,
  not two completed generations or permission to run V2.
- The read-only bridge from #325 verifies one stable V1 snapshot, but can
  neither prevent an ABA change between reads nor fence another writer.
- #324 provides a deterministic *hypothetical* V2 Task identity; it is
  unconnected to the current runtime/Task dispatcher and does not authorize
  real execution.

Therefore **a separate, independently writable V2 SQLite sidecar cannot
fence current V1 writers**. It is unsafe to call it a migration just because
its own transaction is durable. This PR models a guarded protocol only.

## 2. Proposed authority layout, not implemented

**One writer, one authoritative versioned V1 sidecar**. Upgrade the actual
V1 cursor sidecar to a new schema version *only after isolated migration and
rollback verification*. The existing V1 binary refuses unsupported future
schema versions: a desired fail-closed property, not evidence that old images
can safely be reactivated after a schema change.

A future schema shall add a migration record in **the same SQLite file** as
the V1 cursor. Every V1 writer path (including seed, replace, stimuli,
scheduler service and any recovery path) must reject writes to the fenced
scope. The migration CAS and fence insertion must occur within the *same*
`BEGIN IMMEDIATE` transaction with an exact full-cursor match, pinned
model/anchor/predecessor evidence, monotonic revision, owner epoch and
operator-approved migration identifier. Merely keeping the work on the
owner thread is not sufficient to protect from an old binary or a second
connection that bypasses the guard.

The future ledger shall persist at minimum:

- immutable cutover candidate canonical JSON and SHA-256 identity;
- full frozen V1 cursor fingerprint, expected revision, anchor and pinned
  model identity, exact bounded-memory digest and V2 Task ID/input SHA;
- monotonic fencing epoch (never reused), status/revision and explicit
  approval reference **from trusted operator policy outside model data**;
- an append-only outcome/rollback tombstone that preserves old V1 lineage
  and any already-visible V2 Task, never deletes their evidence.

Never use a model-derived text string, GitHub event, or a digest-shaped
candidate ID as an approval token.

## 3. Frozen candidate and monotonic phase simulation

`make_local_goose_v2_cutover_candidate` is pure: bind the full canonical
V2 preview to #325's read-only V1 bridge evidence and the exact V1 dormant
cursor, matching revision, generation, anchor, predecessor result, pinned
model, memory digest, count, omission flag and timestamp fields. The
candidate is bounded, canonical JSON and deterministic. **No Task is stored.**

`simulate_local_goose_v2_cutover` is a pure state machine with stages:

```text
proposal --[fence CAS]--> fenced --[durable freeze]--> frozen
   |                     |                           |
   +--> aborted          +--> aborted                +--> published
                         |                           |        |
                         +--> quarantined             |        +--> quarantined
                                                     +--> quarantined
```

The code requires a monotonic simulated CAS revision, stable V2 identity and
an epoch claimed by the caller. An exact Task ID+input digest is required
to simulate the publish acknowledgment; a replay with the same receipt is
a duplicate, not a second Task. It refuses publish-before-freeze, stale
epochs, changed receipts, abort-after-freeze, reinstate-after-publication,
or progression out of quarantined/aborted.

**The simulator's parameters are not real CAS or TaskStore receipts.**
An untrusted caller can invent them; no production code may treat the
simulator's `accepted` as proof of persistent authorization. It is a
contract for future transaction-backed integration, not a task runner.

## 4. Crash/recovery matrix (future implementation obligations)

| Durable point | On restart | V1 policy |
| --- | --- | --- |
| Before fence CAS | Ignore uncommitted preview; explicit approval required | Keep dormant |
| Fence CAS committed, candidate not yet frozen | Inspect persisted migration row and same V1 cursor; block on mismatch | V1 writes fenced |
| Frozen V2 ID recorded, Task not found | Reconcile exact Task ID and idempotency key; repeat **only the same** Task submission under the same fence/owner epoch; never derive a new due time or memory | V1 writes fenced |
| TaskStore persisted, ledger publish receipt missing | Look up exact Task ID and exact canonical definition; finish ledger receipt only if it matches | V1 writes fenced |
| Task execution/lease in progress or result persisted | Use existing TaskStore lease recovery and exact identity; never trigger another generation or provider | V1 writes fenced |
| Conflicting Task, lost fence, schema mismatch, stale owner, ambiguous historical result | Fail closed, persist quarantine if safe or stop for operator recovery; do not regenerate or delete evidence | No automatic V1 reactivation |
| Published V2 or later | **No automatic rollback to V1 dormant**; explicit separate repair protocol, preserving V2 Task lineage | V1 remains fenced |

The two durable stores impose a **saga/reconciliation**, not an unqualified
single atomic commit. Exactly-once *inference* is not asserted by deterministic
Task IDs alone; the future runner's lease and retry semantics need their
own validation.

## 5. Rollback: reversible before the freeze boundary only

- **Before freeze**: a proposal may be abandoned. After fence but before
  freeze, an operator-approved abort may atomically release that same V1
  fence **only if no V2 publish permission or Task write was ever granted**;
  retain a monotonic aborted tombstone.
- **After freeze**: even if a Task lookup temporarily returns “not found”,
  this is **not sufficient proof of non-publication** after a crash. Abort is
  prohibited by the pure model. Preserve the fence, resume exact-ID recovery
  or quarantine for operator intervention.
- **After publish/execution**: quarantine is an irreversible safety state,
  not a rollback restoring V1. Any real compensation, retirement or V1
  reactivation requires a separate reviewed migration ledger and audit.
- **Never** mutate V1 historical Tasks/results, silently reseed generation,
  remove V2 Tasks or pretend a completed V2 generation was V1.

## 6. Adversarial acceptance gates before any deployment

1. Fake-store source tests for candidate replay/digest, stale full cursor,
   modified anchor/model/memory, wrong V2 schema, stale fence epoch, wrong
   publish receipt, duplicate confirmation, prefreeze abort, forbidden
   postfreeze abort and terminal quarantine.
2. A new *transactional* test against isolated copies of the same V1 SQLite
   sidecar with two competing writer connections and an old V1 binary.
   Prove all cursor writes are fenced or safely fail closed.
3. A dual-store crash injection matrix at **every** SQLite commit,
   TaskStore save/lease/ack and model invocation boundary. Verify exact-ID
   reconciliation and zero double-submission, including power-loss-style
   reopen, under the fixed pinned image and no network/provider.
4. Explicit operator-controlled approval and fresh isolated Fedora proof,
   followed by a separately authorized production deployment; V2 activation,
   provider/Task submission and real wake are **not** authorized by this PR.

The phase simulator is not a replacement for steps 2–4 and must not be
linked into a production migration path without them.
