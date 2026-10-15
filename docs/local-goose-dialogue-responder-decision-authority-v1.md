# Local Goose dialogue responder decision authority v1

Status: design contract only. No canonical stage number is assigned.

This document defines the next authority layer above the already-proven durable responder observer, finite lease, per-event intent/disposition and multi-intervention mechanics. It authorizes no production effect, no completion ACK, no lease, no intervention intent, no Task, no provider call and no autonomous-cycle/stimulus/wake action.

## Current proven checkpoint

The current production responder sidecar is schema v2. `system-responder-v1` is durably at cursor 3 after the explicitly authorized structural retirement of system-authored completion sequence 3. Completion sequence 4 is the current human preferred-head event and remains pending by design.

The isolated multi-intervention proof has established that one finite lease may safely bound more than one intervention when every human event receives a fresh explicit intent, intervening system completions are structurally retired, no system completion can self-chain, quota/interval/expiry fail closed, and no lease is renewed automatically.

A subsequent code-only hardening established crash-safe pairing between a terminal responder intent and its per-event intervention disposition. None of those changes grants a production decision authority.

## Problem being solved

The existing dispatcher correctly revalidates canonical evidence, but two policy questions remain deliberately external to it:

1. who grants the finite authority envelope represented by a lease;
2. who decides that one exact current human event should receive one exact intervention message.

Today the typed responder surface can create a lease and prepare an intent, but the durable lease itself is not linked to a separate grant cause, and the caller's intervention request does not durably preserve the complete evidence snapshot on which the caller says it decided.

That is acceptable for the first bounded proof, where a human gate and fixed script supplied the surrounding authority. It is not sufficient for a durable long-running operational mode.

The next contract therefore makes both decisions explicit durable causes while preserving the existing dispatcher as the final independent validator.

## Core invariants

The system must never implement any of these implications:

`completion event exists -> decision to intervene`

`observer classified current_human -> decision to intervene`

`active lease exists -> decision to intervene`

`previous intervention completed -> next intervention decision`

`lease grant prepared -> lease automatically activated`

`restart -> create missing grant, lease or decision`

The intended decomposition is:

`canonical completion/head evidence -> observable snapshot`

`explicit grant authorization -> durable lease grant`

`explicit grant activation -> finite responder lease`

`explicit per-event intervention decision -> durable decision record`

`decision + still-valid lease + still-exact canonical evidence -> responder intent/disposition`

`intent/disposition -> existing bounded dispatch/accounting/ACK path`

Observation, grant, lease activation, per-event decision, intent admission and dispatch remain independent state transitions.

## Initial decision authority

Decision authority v1 remains manual/typed. No model, provider, autonomous planner, completion observer, cycle worker or previous responder turn receives authority to create a lease grant or an intervention decision.

The initial authority source is a local typed operator action accepted by an explicitly enabled responder control surface. The durable record may identify this source as a fixed server-side value such as:

`local-typed-operator-v1`

That value describes the authority channel. It must not falsely claim a cryptographically authenticated human identity.

A caller-supplied label such as `bertrand` or `sol` is not, by itself, proof of who invoked an AF_UNIX socket. If future policy requires durable personal attribution, the transport must first bind an authenticated principal, for example through kernel peer credentials plus an explicit configured UID policy. That is outside decision-authority v1 unless implemented and proven separately.

## Durable lease grant

A lease must no longer be treated as its own unexplained authority cause in long-running mode. A separate immutable grant record should authorize the exact maximum envelope from which one lease may be activated.

A grant binds at least:

- deterministic `grant_id`;
- bounded caller request/idempotency id;
- fixed server-side `authority_source`;
- `thread_alias`;
- admitted `speaker_kind=system`;
- admitted `speaker_id`;
- exact allowed message-kind set;
- bounded exact purpose bytes;
- maximum `max_system_turns`;
- maximum lease TTL;
- minimum interval;
- creation time;
- grant expiry time if shorter than the allowed lease lifetime;
- durable state and terminal reason/time;
- optional activated `lease_id`.

Recommended grant states are:

- `prepared` — durable authorization exists but no lease exists;
- `activated` — exactly one lease has been created from it;
- `revoked` — grant cannot be activated;
- `expired` — grant can no longer be activated;
- `manual_review` — durable evidence became ambiguous.

A prepared grant is not an active lease and has no completion-feed authority.

## Grant identity and idempotency

Grant identity should be deterministic from the authority channel, caller idempotency key and exact requested envelope. Repeating the same request produces the same grant. Reusing the caller idempotency key with different bounds conflicts.

Times that are assigned by the service should not make an otherwise exact retry become a different grant identity. The implementation may store issue/expiry timestamps as mutable creation evidence while deriving identity from stable request fields and relative bounded TTL.

A grant may activate at most one lease. A retry of activation must reconcile to the same lease. It must never create a replacement lease after exhaustion, expiry or revocation.

## Explicit lease activation

Grant creation and lease activation are separate typed actions.

Recommended flow:

1. operator prepares one exact grant;
2. read-only inspection shows the grant and confirms no lease was created;
3. operator explicitly activates that grant;
4. the store atomically links the grant to exactly one finite lease or returns the already-linked lease on exact retry.

Crash after grant creation but before activation leaves only the prepared grant. Restart must not activate it.

Crash during activation must be reconciled by deterministic grant/lease identity. Recovery may finish bookkeeping for a lease already durably created by that exact activation, but must never infer a new activation merely from a prepared grant.

The activated lease may be equal to or stricter than the grant envelope. It must never widen it.

## Durable per-event intervention decision

An intervention decision records what the external decision authority actually chose before an intent exists.

It binds at least:

- deterministic `decision_id`;
- fixed server-side `authority_source`;
- `grant_id` and active `lease_id`;
- responder `consumer_id`;
- exact completion `sequence`;
- exact completion `event_id`;
- exact completion `result_sha256`;
- exact completion `thread_alias`;
- exact completion `thread_revision`;
- exact completion `task_id`;
- observed completion `speaker_kind` and `speaker_id`;
- exact expected preferred-head revision;
- exact expected preferred-head Task ID;
- exact responder `speaker_kind=system` and `speaker_id` inherited from the lease;
- exact `message_kind`;
- bounded exact message bytes;
- requested finite intent TTL;
- creation time;
- durable state and terminal reason/time;
- optional bound `intent_id`.

The decision must refer to a human-authored event that the caller explicitly inspected. Merely supplying a completion sequence is not enough for the durable decision record.

## Decision identity and idempotency

Decision identity should be deterministic from the grant/lease identity, exact event evidence, expected head evidence, exact actor/message kind and exact message bytes.

An exact replay is idempotent. Different content, a different expected head, a different result hash or a different event under the same caller request identity conflicts.

Server-assigned timestamps do not change decision identity.

## Decision states

Recommended states are:

- `prepared` — explicit decision exists, but no responder intent is bound;
- `intent_bound` — exact responder intent/disposition has been prepared from this decision;
- `completed` — bound intent/disposition reached completed intervention state;
- `conflict` — canonical evidence or authority no longer matches before binding;
- `expired` — decision or lease expired before binding;
- `manual_review` — ambiguous durable evidence exists.

Preparing a decision does not ACK the trigger, consume a lease turn, create a Task or advance the preferred head.

## Decision -> intent admission

Binding a prepared decision to an intent is a separate typed action.

The responder must independently re-read and validate:

- the decision is still `prepared`;
- its grant is `activated` and exactly linked to the supplied lease;
- the lease is active, unexpired, within quota and interval;
- the lease envelope admits the exact system actor/message kind;
- the responder consumer's next event exactly matches sequence/id/hash/task/revision/speaker evidence stored in the decision;
- that event is human-authored;
- the preferred head exactly matches the expected revision/Task in the decision;
- the decision's message bytes and TTL are within all bounds;
- no different durable disposition already owns the event.

Only then may the existing responder intent/disposition be prepared and linked back to the decision.

The dispatcher remains authoritative for canonical validation. A durable decision is authority to attempt intent admission, not proof that canonical evidence is still valid.

## Exact-evidence typed surface

The current responder prepare surface accepts the lease identity, completion sequence, message kind, message and TTL while the dispatcher loads the rest of the evidence itself.

Decision-authority v1 should instead expose a typed decision-preparation surface whose request carries the exact evidence shown by the read-only status/inspection surface:

- completion sequence;
- event id;
- result SHA-256;
- thread revision and task id;
- expected preferred-head revision and task id;
- lease/grant identity;
- exact message kind and bytes;
- finite intent TTL.

The service re-reads all evidence and rejects any mismatch. This ensures the durable record proves which snapshot the external decider acted on rather than only which sequence it named.

## Multiple interventions under one grant/lease

A grant may authorize a lease whose finite `max_system_turns` is greater than one. That does not authorize a sequence of automatic replies.

For every eligible human event:

1. observer/status exposes exact current evidence without mutation;
2. external authority explicitly prepares a fresh decision for that event;
3. decision is explicitly bound to one intent/disposition only if evidence still matches;
4. existing dispatch path may submit/account/ACK exactly that trigger;
5. resulting system completion cannot create a decision or intent;
6. observation-only policy may structurally retire that system completion;
7. the next human event requires another fresh explicit decision.

Neither a grant nor a lease may create a future decision template that auto-applies to later human events.

## Relationship to explicit decline

`explicit_decline` remains a separate non-intervention decision and disposition path. Absence of a grant, absence of a lease, an expired grant, an exhausted lease or absence of an intervention decision never implies decline.

A future unification may introduce a generic human-event decision record with variants `intervention` and `decline`, but v1 should not broaden the already-proven decline path merely for schema elegance.

## Observation boundary

The observer must have no API reference capable of preparing grants, activating leases, preparing decisions, binding decisions to intents or dispatching intents.

Observer code may only:

- inspect fixed-consumer completion/head evidence;
- retire structurally non-intervenable events when observation is explicitly armed;
- apply an independently supplied exact explicit decline through its existing typed path.

It may expose evidence needed by an external decider, but exposure is not authority.

## Recovery ordering

A future worker/runtime using this contract should recover in this order:

1. validate responder schema and fixed consumer identity;
2. normalize expired active leases without replacement;
3. reconcile already-existing responder intents/dispositions;
4. reconcile terminal intent/disposition pairing;
5. reconcile activated grant <-> lease links already durably created;
6. reconcile prepared decisions only as records; do not bind them automatically;
7. recover observation-only dispositions/cursor causality;
8. only then expose current read-only status.

Restart must never:

- activate a prepared grant;
- create a lease because a grant exists;
- prepare a decision because a human event exists;
- bind a prepared decision to an intent;
- dispatch a bound intent unless an explicit execution request is replayed/reconciled under the existing dispatcher contract;
- renew/replace a terminal lease;
- ACK a held human event.

## Read-only operational status

The durable status surface should make authority gaps visible. In addition to the existing observer information, it should report, without mutation:

- current prepared/activated grant for the preferred thread, if any;
- active/terminal lease and remaining quota/expiry/next eligibility;
- current human event exact decision evidence;
- any prepared/intent-bound/terminal decision for that event;
- bound intent/disposition state if present;
- the precise reason no next action is authorized.

Examples of useful status reasons:

- `held_current_human_no_grant`;
- `held_current_human_grant_not_activated`;
- `held_current_human_no_decision`;
- `decision_prepared_waiting_intent_bind`;
- `lease_interval_not_elapsed`;
- `lease_exhausted`;
- `decision_evidence_stale`;
- `manual_review_required`.

Status inspection must never normalize state if doing so would create authority. Pure expiry normalization, if retained, belongs to an explicit recovery/mutation path rather than a supposedly read-only status call.

## Schema direction

A later implementation may extend responder schema v2 with two new durable tables/record types:

- lease grants;
- intervention decisions.

That likely requires an explicit responder schema v2 -> v3 migration. As with the previous v1 -> v2 migration:

- ordinary open must not silently migrate production;
- migration must be explicit and separately authorized;
- existing leases/intents/dispositions must be preserved exactly;
- no fake historical grant or decision may be synthesized for older records;
- historical completed intervention state remains readable as legacy evidence;
- production migration, code deployment and authority activation remain separate gates.

The final schema number and exact SQL are implementation details; this document does not perform or authorize migration.

## Compatibility with existing responder commands

Existing raw lease-create/prepare/dispatch typed commands were sufficient for bounded proofs and may remain available for compatibility in explicitly legacy/test contexts.

Long-running operational mode must not expose those raw authority-creating commands as an equivalent bypass around grant/decision records. The later implementation must choose one of these fail-closed strategies:

- disable raw lease-create/intent-prepare when decision-authority mode is enabled; or
- require those commands to carry and validate exact grant/decision ids and evidence.

Dispatch of an already-bound exact intent may continue to use the proven dispatcher path.

## Autonomous decider remains out of scope

This contract intentionally stops at an explicit external typed decider.

A future model/policy/planner may propose a grant or intervention decision, but giving it authority to durably prepare either record is a new security boundary. Such a design must separately define:

- planner identity and durable proposal identity;
- data/context it may inspect;
- message-generation bounds;
- rate/quota budgets independent of the responder lease;
- approval/escalation policy;
- provider/model authority, if any;
- adversarial tests and production activation gate.

Nothing in decision-authority v1 grants that authority.

## Production defaults

Even after implementation code is present in a production image:

- no grant exists automatically;
- no lease is activated automatically;
- no decision is prepared automatically;
- no intent is prepared automatically;
- no Task is submitted automatically;
- no human completion is ACKed automatically;
- observer activation does not imply decision authority;
- provider remains OFF;
- network remains `none`;
- autonomous cycle and stimuli remain unchanged.

At the current production checkpoint, completion sequence 4 must remain pending unless a future separately authorized decline or intervention flow explicitly acts on that exact event.

## Safe implementation order

No canonical stage number is assigned. A safe order is:

1. **Grant/decision store contract** — deterministic records, schema/migration code and adversarial tests only; ordinary open cannot migrate; no runtime wiring.
2. **Typed authority service** — grant prepare/activate and decision prepare/bind surfaces, disabled by default; exact-evidence requests; no automatic observer coupling.
3. **Isolated proof** — copy of current production checkpoint; prove prepared grant does nothing, explicit activation creates one finite lease, each human intervention needs a fresh exact decision, restart never auto-activates/binds, system events cannot decide, and production is untouched.
4. **Production code deployment** — code/schema capability only, authority mode disabled, no migration/effect.
5. **Explicit production migration** — if schema migration is required, migrate only under a separate gate and leave sequence 4 held.
6. **First operational authority activation** — separate human authorization for one tightly bounded grant/lease/decision on an exact event.
7. **Any autonomous decider** — separate future contract and gate.

Every runtime-bearing tranche follows:

`code -> CI green -> merge -> Fedora integration/deployment -> real Fedora proof`

## Non-goals

This design does not authorize or design:

- autonomous intervention decisions;
- model-generated grants;
- provider/OpenAI activation;
- network access;
- cycle/wake/stimulus authority;
- shell/host command execution;
- automatic decline;
- automatic lease renewal/replacement;
- preferred-head rebase after conflict;
- branching dialogue on conflict;
- implicit ACK of the current human completion;
- use of the manual `sol` consumer cursor as responder authority.

The safe default remains: incomplete evidence or incomplete authority means no action.
