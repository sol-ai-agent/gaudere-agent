# Resume-after-wake cognition authority v0

Status: **DESIGN ONLY / PROVIDER-FREE PREPARATION**  
Issue: #84  
Depends on: first real production WakeIntent PASS and #81 runtime-downtime PASS  
Production authorization: **none**

## Purpose

WakeIntent v0 is intentionally inert. A durable `fired` row proves only that a previously accepted wake deadline became due. It does not create a Task, call a provider, execute an Action, or grant an external effect.

The next experiment needs a separate authority boundary that can eventually resume a cognition thread chosen before the wake. This document defines that boundary without weakening WakeIntent v0 and without authorizing provider call #5.

The core design decision is deliberately small:

> **The durable resume Task is the one-shot claim of resume authority. No new resume table is required for v0.**

The existing Task store already supplies durable identity and idempotency. The existing provider path already supplies a second, external-effect marker through its durable provider Action and consumes provider budget before invocation. The resume boundary therefore composes existing primitives instead of inventing a parallel effect ledger.

## P0 fence

There are three distinct layers and they must stay distinct:

1. **Wake evidence** — `wake_intents.status=fired`; inert and provider-free.
2. **Resume authority** — validation of one fired lineage followed by idempotent creation of one deterministic resume Task.
3. **Provider effect** — later execution of that Task through `ProviderTaskHandler`, which consumes budget and persists an Action effect marker before provider invocation.

A fourth layer, **acting on the cognition result**, is explicitly outside this design. A successful resume cognition result is a proposal/continuation objective, not authority to execute arbitrary external action.

No code path may make layer 1 call layer 3 directly. Any future automatic path must cross layer 2 by durably creating the deterministic Task first.

## Why the Task row is the resume claim

`gaudere::work::Runtime::submit` accepts only valid pending Tasks and rejects an existing Task ID or idempotency key as a duplicate. SQLite additionally constrains `tasks.id` as the primary key and `tasks.idempotency_key` as unique. The current runtime has one database-owning process and one durable mutator path. Under that ownership invariant, a deterministic Task gives the required one-shot claim:

- crash before Task save: no claim exists; retry is safe;
- crash after Task save but before notification/reply: claim exists durably; retry observes a duplicate;
- restart/reopen: the same pending/running/terminal Task is rediscovered;
- no delete/recycle path is introduced;
- no external effect has occurred merely because the Task exists.

A duplicate is accepted as the same claim only after reloading the existing Task and proving that its complete immutable definition matches the canonical expected resume Task. A same-ID or same-key record with a different kind/input/limits/identity is a conflict and fails closed; generic `Runtime::submit == duplicate` alone is not sufficient evidence.

This design is not valid for a future multi-process/multi-writer runtime without an atomic claim/insert primitive. Multi-writer support is a new design boundary.

## Fixed identity

Initial v0 uses one deterministic lineage:

- wake scope: `cognition.reflect.wake.v0`;
- resume task kind: `cognition.resume-after-wake.v0`;
- resume task ID: `cognition.resume-after-wake.v0:<wake-id>`;
- resume task idempotency key: `cognition.resume-after-wake.v0:<wake-id>`.

The source Task and wake are not copied into mutable identifiers. The wake ID must equal the persisted wake ID and the wake's source Task ID must remain the canonical source relationship already validated by ExplicitWake.

The provider Action identity remains derived by the existing provider handler from the resume Task idempotency key. Thus one wake lineage maps deterministically to one resume Task and, if later invoked, one provider Action/budget idempotency key.

## Eligibility

A resume claim may be created only when all of these are true:

1. the resume capability is explicitly enabled in that runtime profile;
2. the inspected wake belongs to fixed scope `cognition.reflect.wake.v0`;
3. exactly one relevant wake record is present for the v0 scope;
4. wake status is `fired`;
5. `terminal_at_ms >= due_at_ms` and source consistency is eligible;
6. wake/source identity is internally consistent;
7. source Task exists, kind is `cognition.reflect.v1`, status is `succeeded`;
8. source Task result content type is the canonical bounded-reflection decision type;
9. its normalized result is exactly a canonical `propose_wake` decision;
10. no conflicting Task exists under the deterministic resume ID or idempotency key.

`revoked`, `manual_review`, `scheduled`, malformed, missing, foreign-scope, foreign-kind, failed, noncanonical, or source-inconsistent records fail closed and create no Task.

A duplicate observation of an already-created **canonically identical** resume Task is success-by-identity, not a new claim.

## Capability shape

### Source/CI phase

The first implementation under #85 must not wire resume authority into production startup at all. The component is exercised only with deterministic stores/fakes or an isolated temporary SQLite database. There is no production flag, profile change, secret, network, or provider object in the provider-free proof.

### Future disabled-by-default runtime phase

A later reviewed slice may introduce a distinct capability such as `--resume-after-wake-cognition`. Absence of that capability means:

- no resume component participates in `WorkController`;
- a fired WakeIntent remains inert forever;
- no Task is synthesized;
- no provider budget or Action row changes.

When the capability is eventually enabled, the resume reconciler must run on the sole worker/mutator path after wake reconciliation and before ordinary dispatch. It may inspect eligible fired evidence and call `Runtime::submit` for the deterministic resume Task. It must not invoke the provider itself.

Production activation of that future capability is a separate gate; #84 and #85 authorize no such profile change.

## Resume context and prompt

The resume Task must not feed arbitrary mutable logs into the model. Its input is built deterministically from durable lineage.

Canonical host-supplied context schema:

```json
{
  "schema": "gaudere.cognition.resume-context.v1",
  "source_task_id": "...",
  "source_decision": {
    "schema": "gaudere.cognition.decision.v1",
    "decision": "propose_wake",
    "reason": "...",
    "wake_after_seconds": 3600
  },
  "wake": {
    "id": "...",
    "accepted_at_ms": 0,
    "due_at_ms": 0,
    "terminal_at_ms": 0,
    "terminal_reason": ""
  }
}
```

The prompt tells Gaudere that this is a bounded continuation of its prior durable intention and that the result is a proposal only. It must not claim that the model remembered anything not present in this context.

The first resume decision schema should be separate from the original wake proposal schema because the WakeIntent v0 lifetime slot is one-shot and may already be consumed:

```json
{"schema":"gaudere.cognition.resume-decision.v1","decision":"stop","reason":"..."}
```

or

```json
{"schema":"gaudere.cognition.resume-decision.v1","decision":"continue","reason":"...","objective":"..."}
```

Rules proposed for v0:

- exact known keys only;
- `reason`: non-empty, <= 1024 UTF-8 bytes;
- `objective` required only for `continue`, non-empty, <= 4096 UTF-8 bytes;
- no shell, URL, credential, arbitrary action, tool invocation, timestamp, or provider field in the decision schema;
- output is a durable cognition proposal only.

A later planner/executor that consumes `objective` is another authority boundary and is not created here.

## External provider effect safety

The current `ProviderTaskHandler` already establishes the correct external-effect ordering:

1. derive deterministic provider Action/idempotency identity from the Task;
2. consume durable provider budget before `provider.invoke()`; the Provider object may already exist from service activation;
3. submit/start a critical Action;
4. persist `effect_started` before `provider.invoke()`;
5. on exception or ambiguous transport, persist unknown/manual-review and never replay automatically;
6. after confirmed provider outcome, persist confirmation before returning the Task result.

The resume design reuses this path. It must not add a second provider invocation path.

This gives two durable markers with different meanings:

- **resume Task row** = internal authority claim that one cognition continuation may be attempted;
- **provider Action effect marker** = external-effect boundary showing that a provider call may have begun.

That separation is the required W30 fence.

## Provider budget behavior

Provider budget policy remains unchanged. The permanent provider call budget must be consumed before invocation and keyed by the deterministic provider Action idempotency key.

The current provider handler treats total/window/cooldown denial as a Task failure rather than a scheduler wait. Therefore the first real resume experiment must be gated so that budget eligibility is checked before enabling/claiming the one-shot resume Task. General autonomous waiting for a future budget window is **not** silently added here; it requires its own scheduling semantics if desired later.

No hidden retry may create a second provider call. `max_attempts=2` may be used only for reconciliation: an existing provider Action causes conservative manual review/no replay under the existing handler.

## Read-only observability

A future `resume-status WAKE_ID` view should derive state from existing durable objects rather than introduce a writer:

- `disabled`: capability absent;
- `ineligible`: wake/source validation fails, with stable reason code;
- `eligible`: fired lineage valid and deterministic resume Task absent;
- `claimed`: deterministic Task exists and is pending/running;
- `completed`: resume Task succeeded and normalized resume decision is durable;
- `failed`: Task failed before an ambiguous external-effect state;
- `manual_review`: Task or provider Action is ambiguous/manual-review.

Report at least:

- wake ID/source Task ID;
- wake due/terminal timestamps and lateness;
- deterministic resume Task ID/idempotency key;
- Task status/attempt count;
- provider Action ID/effect state when one exists;
- provider budget is reported separately, never mutated by status inspection.

Status inspection never creates a Task, Action, provider call, or scheduler event.

## Runtime ordering

If automatic resume is later enabled, one worker cycle should conceptually order:

1. reconcile durable wakes;
2. reconcile resume eligibility/claim by deterministic Task submission;
3. recover expired Task leases;
4. dispatch ordinary pending work;
5. derive next deadlines.

This preserves the single-mutator model. The resume reconciler is not a scheduler callback and does not mutate WakeIntent. Its only allowed mutation is idempotent Task submission.

A future quiescence rule may be imposed for the first real experiment to prevent unrelated pending work from obscuring causal evidence.

## Boundedness

Initial v0 is bounded by construction:

- one fixed wake scope;
- one accepted wake per v0 database scope;
- one deterministic resume Task per wake;
- one deterministic provider Action/budget key per resume Task;
- one bounded provider result;
- no automatic downstream action from the result;
- no automatic second wake.

The resume Task is never recycled after failure/manual-review/completion. Raising any of these limits is a later explicit design change.

## Failure policy

Fail closed whenever identity, source, durable state, schema, provider effect state, or ownership is ambiguous.

Automatic replay is allowed only before any ambiguous external-effect boundary and only when the durable deterministic identity proves that no distinct claim/call can be created. Once an existing provider Action indicates that an external effect may have begun, the current provider path's manual-review/no-replay behavior wins.

Never reconstruct a missing successful provider output by making another provider call.

## Implementation split

### A — #84 design only

- this design;
- crash/adversarial matrix;
- review that no new schema is required for the first claim primitive;
- review source/result schemas and deterministic identities.

### B — #85 provider-free claim proof

- `ResumeAfterWake` validation/Task factory/reconciler with fake or isolated stores;
- deterministic resume Task construction;
- disabled-capability no-effect proof;
- duplicate/restart/reopen proof;
- negative lineage matrix;
- synthetic crash windows around Task submission;
- read-only derived status;
- no provider object/network/secret and no production wiring.

### C — future provider integration, still disabled by default

- bounded resume prompt/result normalizer;
- compose the existing `ProviderTaskHandler` rather than adding provider code;
- provider fake only until a separate real gate;
- explicit tests for Action/budget ambiguity inherited from the provider path.

### D — #86 future real gate

Only after A–C PASS: freeze exact lineage/runtime/image, choose isolated or production target explicitly, preflight budget, review capability profile, and obtain separate authorization for one permanent provider call.

## Explicit non-goals

This document does not authorize or design:

- poweroff/reboot/logout or any disruptive host experiment;
- production profile edits;
- provider call #5;
- a second production WakeIntent;
- a general autonomous scheduler for provider budget windows;
- automatic shell/network/tool execution from a cognition result;
- multi-process writers;
- deletion/recycling of wake, resume Task, Action, or budget evidence.

## Acceptance decision

The proposed v0 boundary is reviewable without a new persistence schema: **one fired wake lineage -> at most one deterministic resume Task -> at most one existing provider Action**. The Task row is the durable internal claim; the existing Action effect marker remains the durable external-effect marker. WakeIntent remains inert and unchanged.
