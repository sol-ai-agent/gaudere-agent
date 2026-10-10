# Local Goose dialogue responder durable policy v1 — observation and authority separation

Status: design contract only, post-Stage 9K5.

This document defines the next durable operating policy for the system responder. It does not assign a new canonical stage number. It authorizes no production effect, no completion ACK, no responder lease or intent, no system-authored dialogue turn, no provider call, no autonomous-cycle mutation, and no stimulus.

The existing bounded responder contract remains authoritative for lease and intent dispatch semantics. This document adds the missing policy above it: how feed observation is separated from permission to intervene, how non-intervenable events can eventually be retired without creating a response, and how multiple bounded interventions remain explicit rather than becoming an automatic reply loop.

## Production checkpoint motivating this policy

After the first real `gaudere-chat` human production turn:

- responder consumer id is `system-responder-v1`;
- responder cursor remains at completion sequence 2;
- sequence 3 is the completed system-authored responder turn from the first production activation;
- sequence 4 is the completed human-authored `gaudere-chat` turn;
- preferred thread `main` is at revision 3 on the human V3 Task;
- the first responder lease is exhausted after exactly one committed turn;
- the first responder intent is completed;
- no active responder lease exists;
- provider remains off and durable provider total remains unchanged;
- the autonomous cycle and stimulus ledger remain unchanged.

Sequence 3 must not be ACKed merely because this document exists. A later explicit activation gate must prove any production cursor movement.

## Core invariant

The durable responder must never implement either of these implications:

`completion event exists -> intervention is authorized`

`previous intervention completed -> another intervention is authorized`

The correct decomposition is:

`completion event -> observable evidence`

`explicit observation authority + structural rule -> possible observation-only retirement`

`explicit finite intervention lease + exact per-event intent -> possible intervention`

Observation, cursor advancement, lease authority, intent admission, Task submission, and subsequent responder turns are separate state transitions.

## Four authority domains

### 1. Completion observation

The completion feed says what canonical preferred-thread work has completed. Reading an event is evidence only and is side-effect free.

A durable observer may inspect only the next event for its own fixed consumer id. It must not enumerate a different consumer's history to manufacture authority.

### 2. Observation-only retirement

ACK is not intervention authority. It is nevertheless a durable side effect because it advances the consumer cursor and makes an event no longer redeliverable to that consumer.

Therefore an automatic observer may ACK only under a separately armed observation policy and only when the event is structurally non-intervenable under this contract. A current human-authored preferred-head event is never automatically ACKed merely because no lease exists.

### 3. Intervention lease

A responder lease is the finite envelope in which system-authored interventions may occur. Existing bounds remain in force: finite TTL, finite turn quota, admitted system speaker, admitted message kinds, minimum interval, one active lease per preferred thread, no auto-renewal.

A lease does not create an intent and does not consume or ACK a completion event.

### 4. Per-event intervention intent

An intent remains the sole durable cause authorizing one system-authored successor. It binds exact completion evidence, exact preferred-head evidence, exact actor/message kind, exact message bytes, deterministic identity, and the finite lease.

No worker may invent an intent merely because a lease exists. No completed intent grants another intent.

## Durable event disposition

A long-running responder needs durable causality for completion events that are consumed without an intervention. A later implementation should extend the responder sidecar with a canonical event-disposition record rather than hiding such ACKs in worker control flow.

A disposition binds at least:

- fixed responder `consumer_id`;
- completion `sequence`;
- completion `event_id`;
- completion `result_sha256`;
- `thread_alias`;
- completion `thread_revision`;
- completion `task_id`;
- observed `speaker_kind` and `speaker_id`;
- a bounded disposition kind;
- the authority source for that disposition;
- creation time;
- durable state;
- optional associated `intent_id` when the disposition is an intervention.

Disposition identity must be deterministic from canonical event evidence plus consumer identity and disposition kind. Replaying the same decision must be idempotent; different decisions for the same consumer/event must conflict.

Recommended disposition kinds are:

- `structural_skip_system` — event is system-authored and therefore cannot trigger a responder successor;
- `structural_skip_stale` — event can no longer be the preferred-head trigger for a CAS successor;
- `explicit_decline` — an explicitly authorized policy/operator decides not to intervene on a current human event;
- `intervention` — the event is bound to exactly one responder intent under a finite lease.

Recommended durable states are:

- `prepared`;
- `acknowledged` for a non-intervention disposition whose feed ACK is durably reconciled;
- `intervention_bound` while an intent is the event's disposition cause;
- `completed` once intervention dispatch/accounting/ACK reconciliation is complete;
- `manual_review` for ambiguous evidence.

The exact schema is deferred to the implementation tranche, but the uniqueness rule is not: one responder consumer/event may have only one durable disposition cause.

## Structural classification rules

The observer evaluates only canonical feed evidence and current preferred-head evidence. It does not call a model or provider to classify authority.

### System-authored completion

If `speaker_kind == system`, the event is never eligible to create another responder intent.

When observation-only retirement has been explicitly activated, the worker may prepare a `structural_skip_system` disposition and ACK that exact next event using the crash-safe ordering below.

This rule prevents responder self-chaining. It applies even if a finite lease still has unused turns. A system response does not grant permission to respond to itself.

### Stale completion

If an event is not the current preferred head and cannot be the exact idempotent recovery target of an already-durable intervention, no new responder successor may be based on it. Preferred-head CAS is final serialization; the responder does not rebase historical events.

Under explicitly activated observation-only retirement, such an event may be retired as `structural_skip_stale` after exact identity/hash validation.

### Current human-authored preferred-head completion

A human-authored event that exactly matches the current preferred head is potentially intervenable. It is therefore held by default.

The observer must not ACK it simply because:

- no lease exists;
- a lease is exhausted or expired;
- minimum interval has not elapsed;
- no message has been supplied;
- the worker has nothing to do yet.

It remains pending until one of two explicit decisions exists:

1. an `intervention` disposition bound to an exact valid intent under a current finite lease; or
2. an `explicit_decline` disposition issued through a separately authorized typed surface.

This preserves the distinction between "not currently authorized to speak" and "durably decided not to speak about this event".

### Unknown or malformed actor evidence

Unknown speaker kind, malformed identity/hash, impossible revision relationship, non-canonical Task evidence, or any ambiguity fails closed. The event remains unacknowledged and the disposition becomes or remains `manual_review` if a durable decision had already begun.

## Observation activation

Code presence is not observation activation.

A production service may contain all observer code while doing nothing. Durable observation mode must be explicitly enabled by a later production gate and must have a fixed consumer id. Enabling observation mode does not create a responder lease.

The initial durable observer authority is intentionally narrow:

- inspect only the next event for `system-responder-v1`;
- validate canonical event identity/hash and preferred-thread evidence;
- automatically prepare/commit only structural non-intervention dispositions;
- stop at a current human preferred-head event;
- never create a lease;
- never invent message content;
- never prepare an intervention intent;
- never submit a Task;
- never call a provider;
- never touch cycle/stimulus/wake state.

A later policy engine that autonomously chooses `explicit_decline` or constructs intervention content would be new authority and requires a separate contract and gate.

## Explicit decline

A current human preferred-head event may need to be consumed without an intervention. That decision must not be inferred from absence of a lease.

`explicit_decline` therefore requires a typed authorization carrying the exact event sequence/id/hash and expected preferred head. It must be durable before the ACK is attempted.

Declining does not consume a lease turn. If a lease exists, it remains subject to its original TTL/quota and is not extended. A decline never renews or creates a lease.

The initial implementation should keep explicit decline manual/typed rather than model-driven.

## Multiple bounded interventions

Multiple interventions may occur under one lease only when its original finite `max_system_turns` is greater than one and all existing lease checks continue to pass.

For each intervention:

1. the next completion event must be a current human-authored preferred-head event;
2. an exact per-event intent must be explicitly prepared;
3. dispatch must use the existing deterministic request id and preferred-head CAS path;
4. exactly one lease turn is accounted only after canonical Task/head reconciliation;
5. the trigger event is ACKed only after that reconciliation;
6. the resulting system-authored completion event must not create another intent;
7. if observation mode is active, that system event may later be retired only by the structural-skip path;
8. the next human event again requires a fresh explicit intent decision.

Thus a lease can bound several interventions without becoming "reply automatically until quota is exhausted".

No intervention may increase `max_system_turns`, extend lease expiry, shorten an already-enforced interval retrospectively, or issue a replacement lease.

## Crash-safe ACK ordering

### Observation-only retirement

For `structural_skip_system`, `structural_skip_stale`, or `explicit_decline`:

1. read exact next event;
2. validate event id/hash/sequence and classification evidence;
3. durably prepare the disposition;
4. re-read next event and revalidate exact identity;
5. ACK exactly that sequence for the fixed responder consumer;
6. durably mark the disposition acknowledged.

Crash before step 5 leaves the event redeliverable and the prepared disposition recoverable. Crash after ACK but before step 6 is reconciled by comparing the cursor and exact durable disposition; it must not create a second decision or intervention.

If the cursor advanced in a way not explained by the durable disposition, fail to `manual_review`.

### Intervention

The existing responder ordering remains authoritative:

1. validate trigger event;
2. prepare durable intervention intent/disposition;
3. submit/reconcile exact V3 successor through preferred-head CAS;
4. durably account exactly one lease turn;
5. ACK exactly the trigger sequence;
6. complete the intent/disposition.

The observer must not separately ACK an event already bound to an intervention disposition.

## Recovery rules

On restart, in order:

1. open and validate the completion, thread and responder stores;
2. normalize expired active leases without creating replacements;
3. reconcile any existing responder intent using deterministic identity and existing dispatcher rules;
4. reconcile prepared non-intervention dispositions against the fixed consumer cursor and exact next event;
5. if any durable cause and cursor evidence disagree, enter manual review;
6. only after recovery may a newly observed event be classified.

Restart must never:

- create a lease;
- create an intervention intent from a pending event;
- convert a structural skip into an intervention;
- rebase a stale intent onto a newer head;
- synthesize a replacement message;
- ACK an unexplained gap.

## Current production backlog semantics

At the post-9K5 checkpoint, `system-responder-v1` is intentionally at sequence 2.

A future observer-activation proof should demonstrate, in isolation before production, that:

1. sequence 3 is recognized as system-authored;
2. no lease or intervention intent is created from it;
3. a durable `structural_skip_system` cause is written before any ACK;
4. only sequence 3 is ACKed;
5. sequence 4 then becomes the next event;
6. sequence 4 is recognized as the current human preferred-head event;
7. it remains unacknowledged in the absence of an explicit decline or valid intervention intent;
8. no Task, provider call, cycle mutation, stimulus, lease or system turn is created by those observation steps.

This document itself performs none of those transitions.

## Authority source for intervention

Durable policy v1 deliberately does not give an autonomous model authority to decide whether to intervene.

The initial authority source remains an explicit typed control action that supplies:

- lease identity;
- exact completion sequence/id/hash expected by the caller;
- exact message kind;
- exact bounded message bytes;
- finite intent TTL.

The dispatcher independently revalidates all canonical evidence. The caller's assertion is not trusted as proof.

A future autonomous policy/planner may propose intervention decisions, but proposal and execution must remain separate. Granting such a planner authority to create intents is a future security boundary requiring its own durable identity, limits, tests and production gate.

## Read-only status surface

A durable responder worker should expose a read-only status that makes blocked authority visible without changing state. At minimum it should report:

- fixed consumer id and cursor;
- next event identity/sequence/actor when readable;
- whether that event is system, stale, current-human, or ambiguous;
- observer mode armed/disarmed;
- active lease identity/state/quota/expiry when one exists;
- prepared/submitted/manual-review intents or dispositions;
- why no action is currently authorized.

Status inspection must not ACK, create leases/intents/dispositions, submit Tasks, or mutate any sidecar.

## Production defaults

Even after durable policy code exists in a production image:

- observer mode is disabled by default;
- no active lease exists by default;
- no completion event is ACKed automatically;
- no intervention intent is created automatically;
- no system successor is submitted automatically;
- provider remains off;
- network remains `none`;
- autonomous cycle and stimuli remain unchanged.

Every expansion of authority is a separate explicit production transition.

## Implementation sequence

No canonical stage number is assigned here. The safe implementation order is:

1. **Disposition store contract** — canonical deterministic disposition identity/schema, uniqueness, crash recovery and adversarial unit tests; no worker and no production wiring.
2. **Observer policy service** — read-only classification plus observation-only retirement for structural skips and explicit decline; disabled by default; no intervention creation.
3. **Isolated Fedora proof** — backlog with system event followed by current human event; prove system skip, stop-on-human, crash/retry/idempotency and zero provider/cycle effects.
4. **Production code deployment** — code/image only, observer still disabled, no cursor movement.
5. **First production observer activation** — separate explicit authorization; ACK only the exact proven system-authored sequence 3 and stop on human sequence 4.
6. **Multi-intervention proof** — only after observer semantics are proven; finite lease >1, explicit intent per eligible human event, system-event structural skips between interventions, no self-chain, no auto-renewal.
7. **Any autonomous intervention decider** — separate future design and gate, not implied by this policy.

Each runtime-bearing tranche must preserve the project closure discipline: code, full CI, merge, Fedora integration, and real Fedora proof before operational closure.

## Non-goals

This policy does not authorize or design:

- OpenAI/provider activation;
- network access;
- autonomous-cycle wake/stimulus authority;
- general host command execution;
- branching dialogue on conflict;
- silent preferred-head rebase;
- automatic lease renewal or replacement;
- model-generated intervention authority;
- automatic response to system-authored events;
- use of the manual `sol` consumer cursor as the responder cursor.

The safe default remains no action whenever evidence or authority is incomplete.