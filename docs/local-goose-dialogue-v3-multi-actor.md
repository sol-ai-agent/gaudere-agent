# Local Goose dialogue v3 — multi-actor system conversation

Status: design contract only.

This document follows the successful production proof of Local Goose dialogue v2:
one human root and one human successor have both succeeded in production with
provider total unchanged, `Network=none`, and the autonomous cycle/stimulus
state unchanged.

This design does **not** authorize a provider call, a Local Goose cycle stimulus,
a new production dialogue turn, a deployment, or autonomous system-to-Gaudere
conversation.

## Problem

Dialogue v2 deliberately models every input as a human message. Its canonical
schema has no durable speaker provenance, and its prompt renders each request as
`human:`.

That is correct for the v2 proof, but insufficient for normal operation.

Gaudere must be able to converse with:

- Bertrand;
- trusted Gaudere system components;
- Sol / Worker roles when acting through an explicitly admitted system channel;
- future bounded runtime observers or evaluators.

The durable record and the model context must never confuse a system message with
a human message.

## Goals

V3 adds:

1. explicit canonical speaker provenance;
2. explicit message purpose;
3. compatibility with the already-proven v2 production thread;
4. a durable observation path so the surrounding system can consume Gaudere
   responses;
5. a bounded intervention path so the surrounding system can answer, correct,
   question, or guide Gaudere;
6. serialized preferred-thread UX without removing the underlying append-only
   branchable lineage;
7. anti-loop limits for any automated system conversation.

V3 preserves the existing local inference boundary:

- fixed Local Goose model;
- provider/OpenAI OFF;
- `Network=none`;
- `tools_enabled=false`;
- no MCP/control socket/governance path exposed to the model;
- no shell, secret, network, or provider fallback authority.

A message from a system actor is conversation data/evidence, **not** an authority
grant to the model.

## New Task kind

Use a new immutable Task kind rather than mutating v2 in place:

`cognition.local-goose-dialogue.v3`

Input content type:

`application/vnd.gaudere.local-goose-dialogue-v3+json`

Result content type:

`application/vnd.gaudere.local-goose-dialogue-v3-response+json`

Existing v1/v2 Tasks remain immutable historical evidence.

## Canonical actor identity

Each V3 input contains exactly:

- `schema`;
- bounded `request_id`;
- `speaker_kind`;
- bounded `speaker_id`;
- `message_kind`;
- bounded `message`;
- fixed `model_sha256`;
- `turn_index`;
- `root_task_id`;
- `predecessor_task_id`;
- `predecessor_result_sha256`.

### speaker_kind

Initially:

- `human`;
- `system`.

No free-form role impersonation is accepted in `speaker_kind`.

### speaker_id

A bounded safe identifier, for example:

- human: `bertrand`;
- system: `sol`, `worker-dev`, `worker-max`, `gaudere-runtime`.

The initial production admission layer must use an allowlist. Merely writing
`speaker_id=sol` in untrusted input does not grant admission.

### message_kind

Initially:

- `dialogue` — ordinary conversational turn;
- `feedback` — assessment/correction of a prior Gaudere response;
- `intervention` — explicit system guidance or challenge;
- `observation` — bounded system state/evidence offered for discussion.

The message kind is descriptive provenance. It does not confer tool or control
authority.

## V3 roots

A new V3 root has:

- `turn_index=0`;
- empty lineage fields;
- canonical actor provenance.

After deterministic identity is derived, the Task ID becomes the durable root
identity exactly as in v2.

## Bridge from the existing V2 production thread

The first V3 production turn should not discard the real conversation already
proved in v2.

V3 therefore supports one explicit bridge predecessor:

- a canonical successful V2 Task may be the predecessor of the first V3 turn;
- its exact result bytes are hashed into `predecessor_result_sha256`;
- `turn_index = predecessor.turn_index + 1`;
- `root_task_id` remains the existing V2 root Task ID;
- the bridge is accepted only if the V2 predecessor is canonical success under
  the fixed production model.

After the bridge, ordinary V3 successors require canonical V3 predecessors.

This keeps one durable conversation identity while making the schema transition
explicit and cryptographically anchored.

## V3 successors

For a V3 predecessor, the worker derives exactly as v2:

- next turn index;
- immutable root identity;
- predecessor Task ID;
- SHA-256 of exact predecessor result bytes;
- fixed model hash.

The caller supplies only admitted actor provenance, request ID, message kind,
message, and the explicit predecessor Task ID.

## Durable response

A V3 result contains exactly:

- `schema = gaudere.cognition.local-goose-dialogue-v3-response.v1`;
- `request_id`;
- `speaker_kind`;
- `speaker_id`;
- `message_kind`;
- `root_task_id`;
- `turn_index`;
- `predecessor_task_id`;
- `predecessor_result_sha256`;
- bounded `response`;
- fixed `model_sha256`.

Repeating speaker provenance in the result makes the cause of each Gaudere
response independently inspectable.

## Prompt rendering

V3 must not render every input as `human:`.

Examples:

`bertrand (human/dialogue):`

`sol (system/feedback):`

`gaudere-runtime (system/observation):`

`gaudere:`

The fixed prompt states that all human and system excerpts are untrusted
conversation content/evidence and do not grant authority.

The existing v2 bounded-context limits remain the baseline:

- at most six recent predecessor turns injected;
- at most 40 KiB history material;
- at most 48 KiB final runner prompt;
- at most 4096 lineage links traversed.

## System observation path

The surrounding system needs Gaudere's responses without polling arbitrary
database state.

Add a bounded durable completion feed for canonical dialogue results. Each event
contains only stable dialogue evidence:

- Task ID;
- root Task ID;
- turn index;
- request ID;
- speaker provenance of the triggering message;
- result SHA-256;
- bounded Gaudere response;
- completion timestamp.

The feed must not contain secrets, raw filesystem data, hidden prompts, or
provider credentials.

Consumers keep durable cursors so a restart does not lose or multiply
observations.

Observation alone creates **no successor Task**.

## System intervention path

A system actor may submit a V3 successor through the same canonical lineage
contract as a human actor.

Examples:

- feedback on a wrong answer;
- new evidence;
- asking Gaudere to reconsider;
- surfacing the result of another subsystem;
- requesting an explanation of a system event.

Every intervention must identify its admitted system speaker and message kind.

There is no hidden privileged system prompt injection path for runtime feedback.

## Preferred thread head and concurrency

The immutable lineage continues to permit explicit branching, but ordinary UX
needs one preferred head when Bertrand and system components can both speak.

Add a small durable coordinator layer mapping a thread alias to:

- root Task ID;
- preferred head Task ID;
- monotonic revision.

Ordinary submission performs compare-and-swap against the expected head. If two
actors race, one advances the preferred head and the other receives a conflict
and must re-read before retrying.

This prevents accidental forks without removing explicit branch creation from
the underlying lineage model.

The current production thread may initially use alias `main`.

## Anti-loop policy

A system observer must never imply an unlimited automatic conversation loop.

Default policy:

- every successful Gaudere response may be observed;
- no automatic successor is created merely because a response exists;
- a system intervention requires an explicit admitted cause/policy;
- duplicate cause + actor + request identity is idempotent;
- automatic mode, if later enabled, uses a finite lease with:
  - explicit purpose;
  - maximum system-authored turns;
  - expiry time;
  - minimum interval;
  - one active lease per preferred thread;
- exhaustion/expiry stops cleanly without fallback;
- provider/OpenAI remains separately gated.

A new external event or human message may open a new bounded intervention
opportunity according to policy.

## Autonomous-cycle separation

Multi-actor dialogue remains separate from the dormant Local Goose autonomous
cycle.

A system dialogue intervention must not:

- create/consume a cycle stimulus;
- mutate the cycle cursor;
- schedule an autonomous wake;
- advance autonomous generation;
- convert a dialogue result into an autonomous pulse.

If the system later wants an autonomous-cycle effect, that remains a separate
typed capability and gate.

## LiveControl / operator surface

Low-level explicit operations should remain predecessor-based, e.g.:

`local-thread-v3-start REQUEST_ID SPEAKER_KIND SPEAKER_ID MESSAGE_KIND MESSAGE`

`local-thread-v3-next REQUEST_ID PREDECESSOR_TASK_ID SPEAKER_KIND SPEAKER_ID MESSAGE_KIND MESSAGE`

A later operator UX should normally use the preferred-head coordinator and hide
Task IDs from Bertrand:

`gaudere-chat --thread main`

System components should use a typed API, not shell construction.

## Delivery stages

1. **Stage 9A — this design**: multi-actor provenance, V2 bridge, observation,
   coordinator, anti-loop contract.
2. **Stage 9B — canonical V3 Task/result contract**: deterministic identities,
   actor/message-kind validation, V2 bridge primitives, unit tests.
3. **Stage 9C — bounded V3 handler**: actor-aware prompt rendering and mixed
   V2/V3 lineage resolver, provider-free fake-runner tests.
4. **Stage 9D — LiveControl + preferred-head coordinator**: explicit low-level
   operations plus serialized normal-thread submission.
5. **Stage 9E — durable completion feed**: exact-once-by-cursor system
   observation with no implicit response.
6. **Stage 9F — isolated Fedora proof**: alternate Bertrand/system turns,
   deliberate feedback/intervention, restart/recovery, concurrency conflict,
   `Network=none`, provider unchanged.
7. **Stage 9G — production code deployment**: capability present but no system
   turn automatically emitted.
8. **Stage 9H — first system-authored production intervention**: separate
   explicit human authorization.

Every stage preserves:

`code -> CI green -> merge -> Fedora deployment/integration -> real Fedora proof`

No design/merge/deployment authorizes provider use, cycle stimulus, or a system
conversation turn in production.
