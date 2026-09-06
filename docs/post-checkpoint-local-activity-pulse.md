# Provider-free post-checkpoint local activity pulse

Status: **DESIGN ONLY / NOT AUTHORIZED FOR PRODUCTION**  
Issue: #152  
Date: 2026-09-06

## 1. Purpose

After cognition call #10, Gaudere completed exactly one provider-free continuity delta checkpoint and then halted provider progression. The prepared successor #11 is durably cancelled with `attempts_started=0` and must not be revived or replayed.

The next capability should therefore prove something different from another cognition cycle: that the persistent runtime can perform a small, useful, observable activity over time **without any provider or external effect**.

This document defines the first such activity as a bounded **local continuity observation pulse**.

The pulse does not think, message, browse, wake an avatar, call a provider, prepare cognition, or mutate host state. It periodically records a deterministic local observation of Gaudere-owned durable continuity facts and then becomes quiescent after a small fixed number of generations.

## 2. Non-authority invariants

The implementation SHALL have no reference or dependency that grants any of the following:

- `Provider` or OpenAI activation/transport;
- provider budget consumption authority;
- provider `Action` creation or confirmation authority;
- `WakeIntent` mutation authority;
- successor/current-cognition creation authority;
- secret source;
- network/HTTP/curl;
- shell/process execution;
- B10 / systemd / Podman / host-control authority;
- Drive, GitHub, Gmail or any other external connector;
- Second Life or avatar authority.

It MAY read only existing Gaudere-owned durable Task, Action, Budget and WakeIntent state and MAY mutate only:

1. its own Agent-owned pulse sidecar; and
2. one local Task/result per admitted observation generation.

Provider total must remain unchanged. The cancelled #11 Task must remain unchanged. Historical WakeIntent rows must remain byte/semantic unchanged.

## 3. Why this is a separate pulse

### 3.1 Do not repurpose `AutonomousCognitionPulse`

`AutonomousCognitionPulse` is explicitly a recurring **cognition preparation** authority. Its cursor contains cognition-specific fields such as predecessor cognition, snapshot Task and current-cognition Task identities. Its service also deliberately stops automatic scheduling when cognition is prepared/waiting because provider execution is a separate authority.

Generalizing that store in place would blur the boundary between:

- provider-free local activity; and
- preparation of a Task that can later become a provider effect.

That coupling is unnecessary and creates regression/authority risk. Keep the existing cognition pulse semantics unchanged.

### 3.2 Do not use WakeIntent

The first real WakeIntent already has historical meaning and the post-checkpoint gate requires WakeIntent state to remain unchanged. A local continuity observation is not an external wake request and should not acquire wake semantics merely to obtain a deadline.

Use the existing in-process exact `gaudere::scheduling::wake::Scheduler` only as a deadline mechanism; do not create or modify `WakeIntent` rows.

### 3.3 Do not reuse `ResumeContextSnapshot` as the durable kind

`ResumeContextSnapshot` is intentionally named and shaped for fresh resume/cognition context. The new activity is not a resume claim and must remain consumable later only through a separate gate. A distinct Task kind avoids semantic backdoors.

## 4. First activity: local continuity observation

Canonical Task kind:

`continuity.local-observation.v1`

Canonical result schema:

`gaudere.continuity.local-observation.v1`

Canonical content type:

`application/vnd.gaudere.continuity-local-observation+json`

The observation is a deterministic, bounded, machine-generated self-audit of local durable facts. It contains no free-form model prose.

Recommended v1 payload:

```json
{
  "schema": "gaudere.continuity.local-observation.v1",
  "scope": "continuity.local-observation.v1",
  "generation": 1,
  "due_at_ms": 0,
  "captured_at_ms": 0,
  "lateness_ms": 0,
  "predecessor_observation_task_id": null,
  "predecessor_observation_result_sha256": null,
  "anchor_checkpoint_task_id": "continuity.delta-checkpoint.v1:...",
  "provider_total": 10,
  "provider_limit": 12,
  "actions_total": 10,
  "actions_confirmed": 10,
  "wake_total": 1,
  "wake_fired": 1,
  "checkpoint_count": 1,
  "latest_checkpoint_task_id": "continuity.delta-checkpoint.v1:..."
}
```

Exact field names may be adjusted during implementation only if the same information/authority boundary is preserved and tests pin the final canonical schema.

### 4.1 Why these facts are useful

They prove that, at a later real time, the persistent runtime can still:

- acquire its own single-owner state safely;
- read the durable continuity/budget/action/wake state it depends on;
- notice that provider budget has not moved;
- notice that the unique checkpoint still exists;
- append one bounded local fact without external help;
- produce an artefact that a future explicitly-authorized cognition gate could inspect.

This is useful operational evidence, not artificial `echo` traffic.

### 4.2 Facts deliberately excluded

V1 should not include:

- hostname, IP, kernel, process list or arbitrary host information;
- secrets or environment variables;
- network state;
- B10 state;
- repository/Drive state;
- arbitrary file contents;
- model-generated text;
- a recommended next action;
- provider request material;
- any command/action payload.

The observation reports continuity facts; it does not decide what Gaudere should do next.

## 5. Bounded proof policy

The first implementation is not an indefinitely recurring daemon policy.

Fixed constants:

- cadence: **24 hours**;
- maximum settled generations: **3**;
- after generation 3: durable state becomes **quiescent** and no further deadline is scheduled;
- disabled by default in source and deployment configuration.

A later gate may revise the generation cap only after the three-generation proof is complete. Merge alone must never make the runtime run forever.

## 6. Durable sidecar

Use a new Agent-owned SQLite sidecar, separate from both Core state and the cognition pulse sidecar.

Recommended scope:

`continuity.local-observation-pulse.v1`

Recommended cursor fields:

- `scope` fixed exact value;
- `revision` CAS revision;
- `generation` last admitted generation;
- `state`: `idle | preparing | settled | blocked | quiescent`;
- `anchor_checkpoint_task_id`;
- `anchor_checkpoint_result_sha256`;
- `anchor_at_ms`;
- `due_at_ms`;
- optional `captured_at_ms`;
- `task_id` for current generation;
- optional `result_sha256`;
- optional `predecessor_observation_task_id`;
- optional `predecessor_observation_result_sha256`;
- `blocked_reason`.

The sidecar SHALL:

- contain exactly one fixed-scope cursor;
- use owner-only filesystem permissions;
- use compare-and-swap replacement by durable revision;
- fail closed on unknown schema, extra rows, invalid state transitions or stale writers;
- expose a strict read-only inspection path that never creates schema/WAL state.

Do not add these scheduling fields to Core schema v4 in the first slice. Keeping them in an Agent-owned sidecar preserves the already-proven production DB contract.

## 7. Seeding / authority boundary

The pulse cannot self-start merely because its binary exists.

Seeding is a separate explicit one-shot operation and requires:

1. source/deployed feature flag enabled for a test environment;
2. exact anchor checkpoint Task ID supplied by the operator/test;
3. the Task exists, is terminal succeeded, is kind `continuity.delta-checkpoint.v1`, and passes strict checkpoint inspection;
4. no existing local-observation cursor exists.

The initial cursor records the exact anchor checkpoint identity and schedules generation 1 at `seeded_at + 24h`.

Production seeding is **out of scope** for issue #152 and for the first implementation PR. A later production gate must separately authorize it.

## 8. Opportunity identity and idempotency

One due opportunity has exactly one Task identity.

Recommended canonical opportunity identity input:

```text
schema=gaudere.continuity.local-observation.identity.v1
scope=continuity.local-observation.v1
generation=<N>
due_at_ms=<fixed due time>
anchor_checkpoint_task_id=<exact anchor>
predecessor_observation_task_id=<previous task or empty>
predecessor_observation_result_sha256=<previous hash or empty>
```

Task ID:

`continuity.local-observation.v1:<sha256(canonical opportunity identity bytes)>`

Important: `captured_at_ms` and the observed facts do **not** participate in opportunity identity. Therefore a delayed/restarted host still maps the same due opportunity to the same Task ID.

Task creation must be create-once/idempotent:

- same ID + exact same durable input/result = duplicate/accept existing;
- same ID + different semantic input = conflict/fail closed;
- never manufacture a second Task for the same generation.

## 9. Scheduling and missed time

Use the existing event/deadline Scheduler. No periodic poll loop.

Normal sequence:

1. cursor `settled/idle` exposes one `due_at_ms`;
2. Scheduler sleeps until that deadline or service interruption;
3. when due, pulse admits at most one generation;
4. one local observation is recorded and settled;
5. if generation < 3, next due time is `captured_at_ms + 24h`;
6. otherwise cursor becomes `quiescent` and no next deadline exists.

### 9.1 Host unavailable past several deadlines

Missed opportunities are **coalesced**, not replayed.

If the host is unavailable for five days, restart performs one observation for the one durable due generation, records its lateness, then schedules the following generation 24h after the new capture time. It must not generate five catch-up Tasks.

This prevents restart storms and turns downtime itself into observable lateness rather than hidden repeated work.

### 9.2 Clock rollback

If `now < last captured/anchor time` beyond exact validation, do not run an observation. Return/record `clock_rollback` as a non-terminal scheduling condition and schedule no earlier than the durable anchor/captured time. Never decrement generation or rewrite an existing Task.

## 10. Crash/restart state machine

Recommended states:

### `idle`

Seeded, no admitted generation yet; waiting for `due_at_ms`.

### `preparing`

Generation and deterministic Task ID have been durably reserved in the sidecar. This state is written before local Task execution.

Recovery rules:

- if deterministic Task does not exist: rebuild the bounded observation and create/execute it once;
- if Task exists terminal succeeded and passes exact schema/identity inspection: adopt it and settle;
- if Task exists non-terminal: recover through existing local WorkRuntime semantics, never create a second semantic Task;
- if Task exists but identity/schema/result conflicts: `blocked` fail closed.

### `settled`

Current generation has one verified terminal local Task/result and its result SHA-256 is bound in the cursor. Schedule the next due time only if generation < 3.

### `blocked`

Durable/manual-review state for invariant conflict, invalid sidecar, impossible Task identity, clock inconsistency that cannot safely self-resolve, or store failure. No deadline while blocked.

### `quiescent`

Three generations settled. No deadline, no mutation, no implicit transition back to active.

## 11. Observation capture transaction boundary

The local observer reads Gaudere-owned state through existing stores while the process holds the normal Agent single-owner state lock.

Recommended order:

1. validate cursor + due opportunity;
2. CAS cursor to `preparing` with fixed generation and deterministic Task ID;
3. read bounded Task/Action/Budget/Wake facts;
4. canonicalize payload with one `captured_at_ms`;
5. create/execute local Task via existing `WorkRuntime` with a dedicated local handler that echoes the canonical payload exactly;
6. re-read/inspect terminal Task;
7. CAS cursor to `settled` with result SHA and next due, or `quiescent` at generation 3.

There must be no provider gate in this path.

## 12. Local Task handler

Implement a dedicated handler, not `local.echo` by name, so task kind and allowed content type stay explicit.

Suggested handler contract:

- accepts only Task kind `continuity.local-observation.v1`;
- validates exact canonical JSON schema/content type/bounds;
- returns exactly the canonical input bytes as successful result;
- no injected services beyond local validation;
- no child Task, Action or WakeIntent creation;
- no filesystem/network/process access.

The useful work happens in the bounded observation builder; the handler makes the result durable through existing WorkRuntime semantics.

## 13. Read-only observability

Add a standalone read-only CLI/inspection path in the implementation slice, for example:

```text
gaudere-agent-local-observation-status \
  --state-dir /var/lib/gaudere \
  --sidecar /var/lib/gaudere-agent/local-observation.db
```

Recommended canonical status JSON fields:

- schema/version;
- enabled/disabled source intent if known from CLI args;
- cursor state/generation;
- anchor checkpoint ID;
- due/captured timestamps;
- current/last observation Task ID;
- result SHA;
- latest observation payload after exact Task inspection;
- next deadline or null;
- `provider_authority=false` as a fixed compile-time/status fact, never inferred from secrets.

The status command must be read-only: no schema creation, WAL activation, Task recovery or pulse advancement.

## 14. Source and deployment gates

The minimal implementation slice SHALL keep the capability inert:

- library/components + tests;
- one-shot/test CLI if needed;
- no default service enablement;
- no production Quadlet mutation;
- no automatic seed;
- default `enabled=false` constructor/config path;
- existing `gaudere-agent.service` behavior unchanged.

A later deployment gate may package a sidecar service, still disabled. A still-later production gate may seed exactly one anchor and prove three generations. These must remain separate changes.

## 15. Test matrix for the implementation issue

At minimum:

### Authority negatives

- provider fake/real objects are not constructible/reachable from the local pulse component;
- no Action rows are added;
- provider budget total unchanged;
- WakeIntent table unchanged;
- #11 Task unchanged;
- no network/syscall/process fixture is needed.

### Disabled/default

- disabled pulse does not seed, create a Task or schedule a deadline;
- unseeded enabled pulse remains inert until explicit seed.

### Seed

- valid exact checkpoint seed accepted once;
- same seed duplicate;
- different anchor conflict;
- non-checkpoint/failed/unknown anchor rejected.

### Due/idempotency

- before due: no Task;
- at due: exactly one deterministic Task;
- repeated observe: same Task, no duplicate semantic work;
- generation increments only after verified settlement.

### Restart/crash

Inject crash hooks after:

1. cursor preparing CAS;
2. Task creation;
3. Task local execution before cursor settlement;
4. cursor settlement before Scheduler re-arm.

Every recovery must converge to exactly one settled Task for the generation.

### Downtime/coalescing

- restart 5 days late produces one observation only;
- lateness is recorded;
- next due = recovered capture + 24h, not the historical missed sequence.

### Clock rollback

- backward wall-clock does not create/decrement/rewrite a Task;
- pulse remains fail-closed until a safe time boundary.

### Generation cap

- exactly generations 1, 2, 3 may settle;
- generation 3 transitions to quiescent;
- no generation 4 Task/deadline exists.

### Corruption/conflict

- extra sidecar row, invalid scope/state/revision, conflicting deterministic Task, bad result content type/hash all fail closed with no new work.

### Read-only status

- status inspection does not modify DB/sidecar mtimes/content and does not create WAL/schema state.

## 16. Recommended minimal implementation follow-up

Create a separate implementation issue after this design is accepted. The first implementation PR should contain only:

1. `LocalContinuityObservation` canonical builder/inspector;
2. `LocalActivityPulseStore` fixed-scope sidecar;
3. `LocalActivityPulse` state machine with three-generation cap;
4. bridge to the existing Scheduler;
5. dedicated local Task handler;
6. read-only status inspection/CLI;
7. deterministic unit/integration tests including injected crashes;
8. documentation proving provider/Action/Wake/external authority absence.

It should **not** include a production unit, Quadlet change, service restart, provider activation or seed of the production sidecar.

## 17. Acceptance decision

This design recommends a **separate bounded local activity pulse** rather than widening cognition or WakeIntent authority.

The first autonomous local behavior is intentionally modest but semantically useful: once per admitted deadline, Gaudere records a durable self-observation that its own continuity state still coheres. Three real generations can later prove that Gaudere remained present and active through time without asking an external model what to do.

Only after that provider-free persistence proof should the project decide whether the next autonomous capability is richer internal goal maintenance, new cognition, external communication, or Second Life presence.
