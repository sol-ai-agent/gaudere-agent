# Bounded reflection v0

## Decision

The next cognition slice is one explicitly submitted, single-provider-call
reflection task. It converts a bounded objective into one strictly validated,
durable decision. It does not create another task, schedule a wake, invoke a
non-provider effect, or enable a background cognition loop.

This is intentionally smaller than a durable continuation engine. It proves the
semantic decision boundary before scheduling is allowed to act on a model result.
SQLite remains at schema v3 in this slice.

## Capability boundary

The application adds a distinct task kind:

```text
cognition.reflect.v1
```

Only an explicit local live-control command may submit it. The existing Unix socket,
mailbox, worker ownership, provider action marker, and durable provider budget remain
the complete execution path. Enabling the OpenAI provider does not generate a
reflection task, and service startup never creates one.

The task may use only the already configured OpenAI Responses provider. It cannot
choose a model, endpoint, secret, task kind, resource limit, network destination, or
external tool. It has no shell, filesystem, social, messaging, or Second Life
capability.

## Input and limits

The operator supplies:

- a safe task identifier;
- one UTF-8 objective of at most 4096 bytes.

The application builds the provider prompt from a fixed instruction envelope and
the objective. The fixed envelope defines the only accepted decision schema and
states that the result is a proposal, not authorization to act.

The reflection task uses hard-coded bounds:

- one provider effect at most;
- the existing global provider budget and idempotency boundary;
- 60 seconds maximum runtime;
- 4096 output bytes;
- two task attempts, where the second attempt is reconciliation only and can never
  repeat an existing provider action.

## Decision contract

The provider must return exactly one JSON object. Two decisions are valid:

```json
{
  "schema": "gaudere.cognition.decision.v1",
  "decision": "stop",
  "reason": "A bounded explanation."
}
```

```json
{
  "schema": "gaudere.cognition.decision.v1",
  "decision": "propose_wake",
  "reason": "A bounded explanation.",
  "wake_after_seconds": 900
}
```

Validation is strict:

- no unknown keys;
- `schema` must match exactly;
- `decision` is only `stop` or `propose_wake`;
- `reason` is a non-empty string of at most 1024 bytes;
- `stop` must not carry `wake_after_seconds`;
- `propose_wake` requires an integer delay from 900 seconds through 86400 seconds.

Successful output is canonicalized and persisted with content type
`application/vnd.gaudere.cognition-decision+json`. Normalized provider usage metadata
is preserved separately without copying the raw provider response.

Malformed JSON or an invalid decision is a definite task failure after a confirmed
provider call. It is not retried. An ambiguous provider effect still enters
`manual_review` through the existing provider boundary.

## Why a proposed wake is not automatic

A model-selected delay is data, not authority. A `propose_wake` result remains
observable durable Task output until a human explicitly accepts that exact source
through the separately gated capability in
[`explicit-exact-wake-v0.md`](explicit-exact-wake-v0.md). Without that capability,
no scheduler deadline is created. Even after explicit acceptance, the deadline can
only become an observable fired wake: it cannot create a successor Task or invoke a
provider.

properties proven together:
Automatic continuation requires a later durable slice with all of these properties
proven together:

- a separate explicit cognition capability gate and permanent operator revoke;
- at most one active run;
- a hard per-run step limit in addition to the global provider budget;
- deterministic successor task identities;
- exact durable wake deadlines with no polling;
- restart reconciliation for every crash point between a completed reflection and
  a successor wake;
- no model control over task kind, prompt envelope, provider, endpoint, or effects.

The explicit wake slice accepts only the canonical normalized `propose_wake`
decision as input and never reinterprets arbitrary model text as a schedule.

## Event and thread model

The AF_UNIX thread only validates, queues, and wakes. The main worker alone submits
the durable reflection task and mutates Runtime/SQLite. Normal dispatch executes the
task synchronously. No timer thread, polling loop, or second database owner is added.

## Validation before any production call

Implementation must first pass deterministic offline tests covering:

- prompt construction and input bounds;
- both valid decisions and canonical output;
- every schema/type/range rejection;
- preservation of provider usage metadata;
- duplicate task submission without a second provider invocation;
- ambiguous provider results entering manual review;
- live-control protocol bounds and disabled-provider rejection;
- the assertion that no successor task or scheduler deadline is created.

Merging and deploying the code does not authorize a production reflection call. A
separate, explicit host validation step will be prepared only after the offline slice
is complete.
