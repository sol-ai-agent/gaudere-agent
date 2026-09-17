# Local Goose self-scheduling loop v1

Status: design contract only. This document does **not** authorize or activate a new production cognition.

## Purpose

The provider-free local continuity pulse was intentionally bounded to three observations and is now durably quiescent. `LocalGooseCognitionService` can reason from the last settled/quiescent observation, but the current v1 cognition Task identity is deterministic from that observation and the model hash. Once that Task is terminal, the same observation cannot legitimately create another cognition without replaying or mutating history.

The self-scheduling loop defined here is the next layer. It must let Gaudere create fresh local cognition opportunities over time while preserving the existing invariants:

- no replay of terminal Tasks;
- no fourth `continuity.local-observation.v1` generation;
- provider/OpenAI execution remains OFF unless a separate external gate is authorized;
- no arbitrary shell;
- local tools remain typed and bounded;
- every new cognition has a fresh durable identity and auditable predecessor;
- deployment alone must not silently create a new production cognition.

The loop is not a resurrection of `cognition.autonomous-pulse.v0`. That older path was designed around provider-era `current-cognition` Tasks and their OpenAI preparation/gate. The Local Goose loop is a distinct provider-free lineage.

## Why the existing Local Goose v1 Task cannot be reused

`cognition.local-goose.v1` is intentionally keyed by:

- one canonical `continuity.local-observation.v1` Task;
- that observation result SHA-256;
- the pinned model SHA-256.

That is correct for the bootstrap path because the same observation/model pair denotes one semantic opportunity. It also means a terminal historical Task must remain terminal. A later implementation must **not** change the v1 identity function, delete a production Task, reset its status, increment its retry budget, or reinterpret a failed v1 Task as a new opportunity.

Fresh recurring cognition therefore requires a new Task kind/schema.

## Proposed durable lineage

Introduce a new Task family:

- kind: `cognition.local-goose-cycle.v1`;
- schema: `gaudere.cognition.local-goose-cycle.v1`;
- result: the existing strict `gaudere.cognition.local-goose.decision.v1` decision schema.

Each cycle Task is immutable and contains enough canonical evidence to define exactly one opportunity:

```json
{
  "anchor_observation_result_sha256": "...",
  "anchor_observation_task_id": "continuity.local-observation.v1:...",
  "captured_at_ms": 0,
  "due_at_ms": 0,
  "generation": 1,
  "model_sha256": "...",
  "predecessor": null,
  "schema": "gaudere.cognition.local-goose-cycle.v1"
}
```

For generation 2 and later, `predecessor` is an object containing the immediately preceding cycle Task ID, its result SHA-256, and its canonical decision. The anchor remains the final canonical local continuity observation and never changes.

The canonical Task ID/idempotency key is derived from the complete canonical input. A different generation, predecessor result or due/capture time is therefore a different semantic opportunity. A restart derives/reuses the exact same Task rather than creating a duplicate.

The recurring cycle Task must not be registered in the generic `TaskDispatcher`; only the dedicated Local Goose loop service may execute it.

## Dedicated sidecar cursor

Do **not** add scheduler state to `local-activity-pulse.db`. That sidecar is deliberately canonical schema v1 with one table and one bounded three-generation cursor. It is historical evidence after quiescence.

Use a new Agent-owned sidecar, proposed host name:

`local-goose-cycle.db`

and container path:

`/var/lib/gaudere/local-goose-cycle.db`

The sidecar contains no provider, Action, secret, WakeIntent or arbitrary command state. It contains one fixed-scope compare-and-swap cursor, for example scope:

`cognition.local-goose-cycle.v1`

The cursor records at minimum:

- revision;
- generation;
- state;
- final local-observation anchor Task/result hash;
- predecessor cycle Task/result hash when one exists;
- due time when scheduled;
- captured time/current Task ID while prepared;
- blocked reason on invariant failure.

Suggested states:

- `dormant`: no self-scheduled deadline exists;
- `scheduled`: one exact future opportunity is armed;
- `prepared`: the exact cycle Task has been durably created and may be executing/recovering;
- `blocked`: a structural/canonical conflict occurred and automatic progress is stopped.

All writes are compare-and-swap by durable revision. The store must be owner-only regular file mode `0600`, use bounded canonical fields, fail closed on schema drift and have a separate strict read-only inspection path.

## Bootstrap gate

The first recurring cognition cannot derive a wake delay from a successful predecessor because production currently has no successful recurring-cycle predecessor. Historical failed `cognition.local-goose.v1` Tasks remain audit evidence and must not be replayed.

Therefore bootstrap is explicit and one-shot:

1. validate that the existing local activity cursor is canonical, generation 3 and `quiescent`;
2. validate its final local observation Task/result hash;
3. seed the new loop sidecar from that anchor in a non-running state;
4. activating the first scheduled opportunity is a separate explicit production gate.

Merely deploying code or creating the sidecar must not run Goose.

After the first successful cycle, future self-scheduling is determined by Gaudere's durable decision rather than by a human cadence.

## `next_wake_after_ms` semantics

`next_wake_after_ms` becomes operational scheduling intent for the **next local cycle**, not an OpenAI action.

Semantics:

- nonnegative integer: request a new local cognition after that delay;
- `null` with `idle`: become `dormant` and wait for a future durable stimulus;
- `null` with `request_openai`: record the request but do not call OpenAI; become `dormant` unless another local wake was explicitly requested;
- `null` with `continue_local`: means continue as soon as the local anti-spin safety floor permits.

A requested delay of `0` has the same scheduling meaning as immediate continuation subject to the anti-spin floor.

The runtime needs a small hard minimum delay between completed local inferences to prevent an accidental CPU-tight loop. This is a resource-safety bound, not a semantic choice for Gaudere. v1 should use a documented constant and no semantic maximum cadence; overflow or an unrepresentable deadline fails closed.

The runtime must never translate `request_openai` into a provider call. The request remains durable evidence until a separate provider gate decides whether execution is authorized.

## Opportunity capture

When a scheduled deadline becomes due, the loop service freezes one `captured_at_ms`, derives one canonical cycle Task and persists it before execution.

The cycle prompt receives:

- the fixed final local continuity anchor;
- the immediate predecessor decision, if any;
- generation/due/capture timing;
- the pinned model hash;
- the same identity/autonomy/security framing already used by Local Goose;
- access only to the current typed-tool surface allowed by the Goose governance policy.

Live facts that already have typed inspection tools should be inspected through those tools rather than duplicated into unaudited prompt prose. The cycle Task itself still carries enough durable evidence to reconstruct why this opportunity exists.

## Crash/restart semantics

The state machine must be restart-safe:

- `dormant`: arm nothing;
- `scheduled`, before deadline: re-arm the exact shared Scheduler deadline;
- `scheduled`, deadline reached: freeze capture once and transition to `prepared` with one exact Task ID;
- `prepared`, Task pending: execute/recover that exact Task only;
- `prepared`, Task succeeded canonically: settle predecessor evidence and derive the next state/deadline from the decision;
- `prepared`, Task terminal non-success: never replay it. v1 fails closed to `blocked`; a later separately designed recovery policy may turn failure evidence into another generation, but it must not silently consume the failed Task's remaining attempt budget.

Expired Work leases are handled only by the existing generic Work runtime recovery rules. The loop must not invent a second recovery mechanism.

Downtime coalesces to one due opportunity: if a scheduled deadline passed while the host was unavailable, startup prepares exactly that one durable opportunity. It must not generate one Task per missed wall-clock interval.

## Scheduler integration

Use the existing in-process shared `gaudere::scheduling::wake::Scheduler` only as a wake mechanism. Durable truth remains in the loop sidecar.

A `LocalGooseCycleSchedulerBridge` should derive at most one exact deadline from the canonical cursor and call `Scheduler::request_at`. `dormant` and `blocked` have no deadline.

The main loop then follows the same pattern already used by the local activity pulse:

1. step the loop service at startup;
2. arm/refresh the Scheduler from the durable cursor;
3. after a Scheduler wake, step at most one state-machine transition;
4. never poll in a tight loop.

## Authority boundary

The Local Goose loop may:

- read the canonical final local continuity anchor;
- read its own predecessor cycle Task/result;
- create exactly the next deterministic local cycle Task;
- execute that Task through the pinned local Goose runner;
- use the already-governed typed MCP tools;
- mutate only its own sidecar cursor;
- arm the shared in-process Scheduler.

It may not:

- call OpenAI/provider transport;
- consume provider budget;
- create/confirm provider Actions;
- revive cognition #11;
- mutate the historical local activity pulse;
- replay any historical Local Goose Task;
- execute arbitrary shell;
- enable network access;
- change B10/systemd/Podman/host state except through separately authorized typed boundaries;
- promote a structural risk-envelope change locally.

## Provider request handoff

A successful `request_openai` decision remains a local Task result. The first implementation should expose it through status/inspection only.

A later gate may define a deterministic provider-request record derived from the local decision. That later gate must preserve all existing provider rules: provider OFF by default, durable 10/12 evidence checked before any provider-capable promotion, no automatic call merely because a request exists, and OpenAI validation for structural risk-rule promotion.

## Staged implementation

Implement in deliberately small provider-free changes:

1. **Cycle contract** — canonical `cognition.local-goose-cycle.v1` Task creation/inspection and prompt construction; no scheduler and no production activation.
2. **Durable loop cursor** — new strict sidecar store plus read-only inspector and transition tests; still no inference.
3. **Scheduler/service** — deadline bridge, startup recovery and FakeRunner tests; provider-free CI only.
4. **Runtime integration** — opt-in CLI/Quadlet plumbing remains absent from production templates until an explicit deployment gate.
5. **Fedora isolated proof** — copied state, new empty/seeded loop sidecar, real local model, typed tools, network none, provider total unchanged.
6. **Production activation gate** — stopped-state backup, explicit seed/activation, exact image provenance and real Fedora proof. This step is not authorized by this design document alone.

At every stage preserve the operational closure rule:

`code → CI green → merge → Fedora deployment → real Fedora proof`

No stage may treat a merge as production closure.