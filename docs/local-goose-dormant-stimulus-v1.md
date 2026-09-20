# Local Goose dormant stimulus v1

Status: design contract only. This document does **not** authorize production
deployment, a new local cognition, a provider call, or a wake.

## Context

The Local Goose self-scheduling loop v1 is now implemented through its explicit
production activation gate. A successful cycle may intentionally settle to
`dormant` when its canonical decision is `idle` with
`next_wake_after_ms=null`, or `request_openai` with no local wake request.

That behavior is correct: `dormant` must not silently become a periodic timer.
The existing loop design says a dormant cursor waits for a future durable
stimulus, but v1 does not yet define such a stimulus.

This document defines that missing boundary.

## Goals

A future implementation may let one explicitly authorized, durable and bounded
stimulus re-arm a canonical dormant Local Goose cycle while preserving:

- provider/OpenAI remains OFF unless a separate provider gate is authorized;
- no arbitrary shell;
- no network authority is added to `gaudere-agent`;
- historical `continuity.local-observation.v1` generation 3 remains immutable
  and quiescent;
- cancelled cognition #11 remains cancelled;
- no historical Local Goose Task is replayed;
- no periodic fallback timer is created;
- a dormant decision remains dormant until a new durable stimulus is accepted;
- every re-armed cycle produces a fresh deterministic Task identity from the
  existing cycle contract;
- all stimulus acceptance and consumption is auditable and restart-safe.

## Non-goals

This design does not:

- decide that production should be re-armed now;
- add a recurring human-defined cadence;
- convert `idle` into `continue_local`;
- call or prepare OpenAI;
- revive `cognition.autonomous-pulse.v0`;
- reuse the historical one-shot reflection wake scope as though it were the
  Local Goose cycle;
- create a generic event bus or arbitrary external payload channel;
- accept free-form prompt text as a stimulus.

## Why the historical explicit wake is not reused directly

The existing `ExplicitWake` / `cognition.reflect.wake.v0` path is a
historical bounded capability tied to `cognition.reflect.v1`,
`propose_wake`, a lifetime maximum of one accepted wake, and a fired state
that deliberately creates no successor Task.

Those invariants are valuable and remain unchanged. They are not the semantic
contract required for a recurring Local Goose lineage. The new mechanism may
reuse generic scheduler/store techniques, but it must use a distinct scope and
must not reinterpret or mutate the historical wake record.

## Durable stimulus ledger

Use a separate Agent-owned sidecar:

`local-goose-cycle-stimulus.db`

The current `local-goose-cycle.db` schema remains unchanged. The stimulus
sidecar is append-only apart from a single bounded terminal transition for each
record.

A v1 stimulus record contains at minimum:

- fixed scope `cognition.local-goose-cycle.stimulus.v1`;
- deterministic stimulus identity;
- source kind selected by application code;
- bounded source identity;
- acceptance timestamp;
- exact cycle cursor revision/generation observed at acceptance;
- status;
- optional consumed timestamp;
- optional resulting scheduled cursor revision;
- optional terminal reason.

Suggested statuses:

- `accepted`: durable stimulus exists and has not yet been applied;
- `consumed`: it successfully re-armed exactly one dormant opportunity;
- `superseded`: the target cursor changed before consumption, so the stimulus
  has no effect;
- `manual_review`: durable ambiguity or invariant drift was detected.

Records are never deleted, recycled, or silently retried against a different
cursor generation.

## First source kind

The first implementation should expose only one source kind:

`explicit_local_recheck`

It is intentionally content-free. It means only: "create one fresh local
reasoning opportunity from the current canonical predecessor."

The acceptance API must not accept:

- arbitrary prompt text;
- a model selector;
- a delay or absolute deadline;
- a shell command;
- a tool name;
- a provider request;
- a filesystem path.

The caller supplies only a bounded idempotency identity for the explicit
request. Application code supplies the fixed scope/source kind.

A later separately reviewed change may add additional typed producers such as a
Second Life event, repository event, or other bounded observation. Each source
kind must define its own canonical durable evidence and cannot inherit authority
merely because the generic ledger exists.

## Idempotency

The explicit source identity is a bounded request identifier. The stimulus ID is
derived from the fixed scope, fixed source kind, source identity, and the exact
target cycle cursor revision/generation.

Retrying the same request returns the original durable record. A source identity
is single-use across the lifetime of the ledger: once used, it never acquires a
new meaning on a later cursor. A new cursor therefore requires a fresh request
identity.

## Acceptance preconditions

The main Agent owner may accept a stimulus only when all of the following hold:

- the Local Goose cycle sidecar is canonical;
- scope is exactly `cognition.local-goose-cycle.v1`;
- state is exactly `dormant`;
- generation is at least 2;
- predecessor Task ID/result SHA-256 are present and canonical;
- the predecessor Task exists in `state.db`, succeeded exactly once, and
  passes `canonical_local_goose_cycle_success`;
- the predecessor result hash matches the cursor;
- there is no accepted stimulus already targeting the same cursor revision;
- provider budget state is not modified by acceptance.

Acceptance persists the stimulus before any in-memory scheduling action.

## Consumption and re-arm

Consumption runs on the sole main worker. It re-reads both durable stores and
requires the exact cursor revision/generation captured by the stimulus.

Acceptance samples the worker clock once and persists that value as
`accepted_at_ms`. Consumption deliberately reuses that durable timestamp so
crash recovery can recognize the exact scheduled successor deterministically.

If the cursor is still the same canonical `dormant` cursor:

1. use the durable `accepted_at_ms` as the immediate scheduling timestamp;
2. construct the existing canonical dormant -> scheduled transition;
3. keep the same generation and predecessor evidence;
4. set `due_at_ms` to `accepted_at_ms`;
5. commit the cycle cursor CAS;
6. mark the stimulus `consumed` with the resulting cursor revision;
7. re-arm the existing `LocalGooseCycleSchedulerBridge`.

The generation does not advance merely because a stimulus is accepted. The
next successful cycle Task settlement remains the only transition that advances
generation.

This deliberately reuses the already-proven Local Goose cycle Task schema.
Fresh due/capture timing yields a fresh deterministic Task identity without
changing historical Tasks.

If the target cursor changed before the cycle CAS, the stimulus becomes
`superseded` and performs no wake.

If the cycle CAS commits but recording `consumed` becomes durably uncertain,
the implementation must fail closed and reconcile by the exact resulting cursor
revision on restart; it must never schedule a second opportunity.

## Crash/restart semantics

The implementation must prove these boundaries:

| Crash point | Durable state | Recovery |
| --- | --- | --- |
| before stimulus commit | no stimulus | caller may retry |
| after stimulus commit, before cycle CAS | accepted | startup retries only against the exact captured cursor |
| after cursor CAS, before consumed marker | scheduled cursor + accepted stimulus | startup recognizes exact resulting cursor and marks consumed; no second CAS |
| after consumed marker | scheduled cursor + consumed stimulus | normal cycle scheduler recovery |
| cursor changed before consumption | accepted + different cursor | mark superseded, no effect |
| invariant ambiguity | manual_review | no automatic wake |

## Scheduler semantics

The stimulus ledger introduces no timer of its own.

An accepted stimulus causes an immediate worker event only so the main owner can
attempt the durable re-arm transition. Once the cycle cursor is scheduled, the
existing Local Goose scheduler bridge remains the sole wake mechanism.

If there is no accepted stimulus and the cycle is dormant, there is no scheduler
deadline and no polling loop.

## Authority boundary

The stimulus path may:

- inspect the current Local Goose cycle cursor;
- inspect the exact predecessor Task/result;
- append one bounded stimulus record;
- CAS one exact dormant -> scheduled transition;
- notify the existing shared Scheduler after the durable transition.

It may not:

- execute Goose directly;
- create a cycle Task directly;
- skip the cycle service;
- mutate historical Tasks;
- mutate the final local observation;
- mutate provider Actions or budget;
- call OpenAI;
- enable network;
- run shell;
- change systemd, Podman, B10, or host state.

## Control surface

The first control surface should be one bounded live-control operation processed
by the sole main worker, conceptually:

`stimulate-local-goose-cycle REQUEST_ID`

The control thread may validate request syntax and enqueue the command, but it
must not touch SQLite or the cycle sidecar.

A B10 operation, if added later, must call this same bounded control surface
rather than gaining direct write access to either sidecar.

## Staged delivery

Deliver this capability in small provider-free slices:

1. **Stimulus contract/store** — strict sidecar schema, canonical identities,
   append/inspect/terminal transitions, deterministic crash tests. No cycle
   mutation.
2. **Cycle re-arm service** — exact predecessor validation and dormant ->
   scheduled CAS with fake clock and restart reconciliation. No live control.
3. **Live-control integration** — one bounded
   `stimulate-local-goose-cycle REQUEST_ID` operation; no B10 yet.
4. **Isolated Fedora proof** — copied production state/sidecars, network none,
   local model not required, provider total unchanged.
5. **Production deployment gate** — code deployment only; acceptance of a real
   stimulus remains a separate explicit authorization.
6. **Optional B10 bridge** — only after the live-control path is proven; typed,
   bounded, no direct SQLite mutation.

At every implementation slice preserve:

`code -> CI green -> merge -> Fedora deployment/proof -> real Fedora proof`

No merge or deployment authorizes a real stimulus.
