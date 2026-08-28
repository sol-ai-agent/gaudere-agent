# Autonomous current-cognition pulse v0

Status: **design only / provider-free**.

Origin: issue #122 after the successful post-promotion production canary (#121, provider call #8).

This document defines how Gaudere can prepare its own next cognition opportunity from its own durable state and clock without an operator constructing a context file or selecting a predecessor. It grants **no provider-call authority, no action authority, no production activation and no call #9**.

## 1. Why this boundary exists

`cognition.current.v0` is now repeatable and production-proven, but each cycle is still assembled externally:

1. an operator chooses the predecessor Task id;
2. an operator/B10 builds a context-request file;
3. a one-shot records the snapshot and claims the Task;
4. a separate one-shot executes the provider call.

The first three steps are not inherently external effects. They can be moved behind a narrow, provider-free durable authority boundary before Gaudere is ever allowed to spend provider budget automatically.

The design deliberately does **not** merge preparation and provider execution. A prepared current-cognition Task remains inert until a later, separately enabled provider authority chooses to execute it.

## 2. Fixed scope and initial bootstrap

The recurring scope is:

```text
cognition.autonomous-pulse.v0
```

The historical `WakeIntent` is not reused as a recurring scheduler. It remains immutable historical evidence.

The autonomous cursor is seeded exactly once from a specifically named, already-succeeded canonical cognition Task. For the first production-capable lineage, the intended seed is call #8:

```text
cognition.current.v0:4062305a3945076197c12e434bb5b3fbca5c390a0ae5f04d50acdf24e1738af4
```

Seeding is a provider-free bootstrap operation. It does not infer a head by scanning or by wall-clock order. After seed, the cursor advances only through an exact prepared Task that later becomes one canonical succeeded cognition result.

## 3. Durable pulse cursor

A recurring authority needs a small mutable durable cursor; forcing this state into immutable work Tasks would either require unbounded scans or make crash recovery depend on reconstructing a head indirectly.

The later implementation should therefore introduce a dedicated store (and, if required by the shared SQLite schema, a separately reviewed schema migration) with one row per fixed scope.

Logical fields:

```text
scope                       fixed primary key
generation                  monotonically increasing integer
state                       idle | preparing | prepared | blocked | quiescent
predecessor_task_id          exact succeeded cognition Task
predecessor_result_sha256    hash of canonical predecessor result bytes
anchor_at_ms                 last accepted state-transition clock anchor
due_at_ms                    next earliest preparation time
observed_at_ms               nullable; frozen first observation of a due pulse
snapshot_task_id             nullable
current_task_id              nullable
blocked_reason               bounded text
```

The cursor is authority state, not model memory and not an Action/effect marker.

Invariants:

- `generation` never decreases;
- one generation can name at most one `observed_at_ms`, snapshot and current Task;
- `predecessor_task_id` always resolves to one canonical succeeded cognition result;
- `prepared` names exactly one canonical pending/running/terminal current-cognition Task;
- failed/manual-review/noncanonical prepared Tasks never become predecessors;
- a cursor conflict or corrupt row blocks the scope rather than repairing it automatically.

## 4. Cadence policy

Initial policy is intentionally conservative:

```text
continue cadence: 6 hours
quiescent recheck after a canonical stop: 24 hours
minimum supported cadence: 1 hour
maximum supported cadence: 24 hours
```

The policy may become configuration later, but production activation must freeze explicit values.

Why 6 hours: the existing provider rolling policy permits at most four new calls per 24 hours. A six-hour cognition cadence cannot exceed that limit under normal operation, while the durable budget remains the final authority.

A pulse is never generated merely because many wall-clock intervals elapsed. `due_at_ms` is anchored to the last accepted lineage transition. Runtime downtime therefore creates **at most one catch-up opportunity**, not one Task per missed period.

## 5. Provider-free observation transition

The pulse component may be called repeatedly by a process loop. Most observations must be no-ops.

### 5.1 Disabled / blocked / not due

- disabled: zero mutation;
- blocked: zero automatic repair;
- `now < due_at_ms`: zero mutation;
- clock before `anchor_at_ms`: fail closed as clock rollback;
- provider budget says a hypothetical next call is not currently admissible: do not prepare a pulse and do not consume budget.

The budget check is read-only. This component never creates an Action or budget consumption.

### 5.2 Freeze the due observation first

When `idle` or `quiescent` becomes due and all read-only gates pass, the first durable mutation is to freeze:

```text
state = preparing
observed_at_ms = now
generation = unchanged
```

This must occur before creating the context snapshot.

That ordering is essential. If the process crashes after freezing the observation but before snapshot creation, recovery reuses the exact same `observed_at_ms`; it must not create a falsely newer memory after restart.

### 5.3 Deterministic local context

The local context is built only from Gaudere-owned facts available without shell, connectors or external network access.

Required v0 facts:

- pulse scope and generation;
- `anchor_at_ms`, `due_at_ms`, frozen `observed_at_ms`, and derived lateness;
- exact predecessor Task id and canonical predecessor decision bytes;
- provider budget snapshot: total used, rolling-window used and whether a hypothetical new consumption is admissible;
- historical WakeIntent summary limited to its fixed durable status/count (observation only; no wake authority);
- current durable schema/version identifier when available through the persistence layer;
- explicit statement that this capsule is observation data, not instructions or action authority.

The capsule remains subject to the existing bounded UTF-8/current-context schema and content-addressed snapshot rules.

No GitHub, Drive, Gmail, B10, arbitrary filesystem, host-process or unrelated Podman facts belong in v0.

### 5.4 Snapshot and claim

The implementation reuses existing components rather than defining a second cognition model:

```text
frozen local context
  -> ResumeContextSnapshotRecorder (with Now fixed to observed_at_ms)
  -> CurrentCognitionCycle.claim(predecessor_task_id, snapshot_task_id)
  -> cognition.current.v0:<existing deterministic linkage hash>
```

After both durable objects validate exactly, the cursor becomes `prepared` and stores their ids.

If the process crashes in `preparing`, restart deterministically reconstructs the same request from the frozen cursor and reuses the same fixed observation clock. Duplicate snapshot/claim results are accepted only after exact immutable-definition validation.

The preparation phase still has zero provider effects.

## 6. Prepared-state settlement

A prepared pulse does not create another pulse until its named current Task reaches a recognized terminal state.

Settlement is provider-free observation of durable state.

### Succeeded canonical cognition

If and only if the exact named Task is `succeeded` with canonical `gaudere.cognition.resume-decision.v1` output:

1. verify its result hash;
2. promote that Task to `predecessor_task_id`;
3. increment `generation` by exactly one;
4. clear snapshot/current ids and `observed_at_ms`;
5. set a new `anchor_at_ms = settlement_now`;
6. set the next due time according to decision policy:
   - `continue`: `anchor + 6h`;
   - `stop`: `anchor + 24h` and state `quiescent`.

This settlement authorizes only a future cognition opportunity. The model result still grants no shell/network/tool/successor action authority.

### Failed, manual-review, ambiguous or noncanonical cognition

Set the cursor `blocked` with bounded evidence. Do not select another predecessor, do not create another current Task and do not retry the provider effect.

A later repair requires a separate explicit authority path.

## 7. Time anomalies

### Runtime down across deadline

On the first safe observation after restart, one overdue generation may enter `preparing` with:

```text
lateness_ms = observed_at_ms - due_at_ms
```

No additional missed generations are created.

### Large forward jump

Same rule: one catch-up opportunity only. Future due time is not calculated until the prepared cognition settles successfully.

### Clock rollback

If observation time is earlier than the durable `anchor_at_ms`, no pulse is prepared. Return an explicit clock-rollback result. Do not move the anchor backward.

A rollback after a due observation has already frozen `observed_at_ms` cannot redefine that generation; recovery uses the frozen value.

## 8. Authority boundaries

The v0 pulse may:

- read Gaudere durable state;
- read durable provider-budget eligibility without consuming it;
- freeze its own cursor;
- create/recover one local context snapshot;
- claim/recover one deterministic `cognition.current.v0` Task;
- settle that exact Task after it becomes terminal.

It may **not**:

- invoke OpenAI or any Provider;
- create or mutate provider Actions;
- consume provider budget;
- enable or create WakeIntent;
- invoke shell or B10;
- read Drive/GitHub/Gmail;
- mutate the host outside Gaudere;
- execute an objective returned by cognition;
- retry a failed/ambiguous cognition effect.

Provider execution remains a later authority layer. Production service wiring remains a still later gate.

## 9. Provider-free proof matrix

The first implementation PR must remain isolated from the persistent service and prove with fake clocks/reopen:

1. disabled => zero new durable state;
2. seeded idle state validates exact predecessor/result hash;
3. before due => zero mutation;
4. first due observation => exactly one frozen `preparing` generation;
5. crash after frozen observation but before snapshot => same observation/snapshot on reopen;
6. crash after snapshot but before claim => same snapshot/claim on reopen;
7. 100 repeated observations => same generation and no duplicate work;
8. downtime across one or many cadence intervals => one late pulse only with exact lateness;
9. large forward jump => one catch-up only;
10. clock rollback => fail closed / no backward anchor;
11. budget unavailable => no preparation and zero budget mutation;
12. canonical latest named predecessor only; failed/manual-review/noncanonical predecessor blocks;
13. local context contains only allowed Gaudere-owned facts and explicit no-authority text;
14. existing `CurrentCognitionCycle` identity is reused unchanged;
15. successful synthetic settlement advances exactly one generation and cadence;
16. canonical `stop` enters 24h quiescent cadence;
17. failed/manual-review/ambiguous synthetic result blocks and never replays;
18. Actions, budget consumptions and WakeIntent remain byte/logically unchanged;
19. no Provider/OpenAI/secret/network/B10/connector dependency;
20. `src/main.cpp`, production Quadlet and service loop contain no pulse wiring in this first implementation.

## 10. Production activation sequence

Not authorized by this design. If provider-free implementation passes, later slices must remain separate:

1. package pulse component with persistent service still disabled;
2. prove fake-provider execution from one pulse-prepared Task;
3. design an explicit persistent-service provider authority/cadence gate;
4. run staging across real downtime and budget-window boundaries;
5. only then consider production activation with a frozen cadence and rollback plan.

The remaining lifetime provider budget is scarce evidence. Automatic cognition must not be enabled merely because the mechanism exists.

## 11. Success criterion

The design succeeds when a later provider-free implementation can demonstrate:

> Starting from one explicitly seeded successful cognition head, Gaudere can notice that its next cognition is due, freeze the moment of observation, build a bounded self-context snapshot and claim exactly one deterministic next cognition Task across crashes and downtime — without an operator constructing that cycle and without spending provider budget.

That is the next autonomy threshold. Provider execution and acting on the resulting objective remain distinct later thresholds.
