# Pre-wake runtime upgrade v0

Status: **PREP ONLY / NOT AUTHORIZED FOR PRODUCTION**.

## Why this gate exists

The current schema-v4 production image is intentionally frozen at Agent
`ae094cefee86a3f6c5d0d4d3f868325f378c9376` / Core
`c24c40b84a12e51515cee4611e3dc79e9fd83892`, with immutable runtime image
`sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3`.
That image contains the inert WakeIntent v0 acceptance/revocation primitives, but it
predates the read-only `wake-status` observability merged later in Agent PR #76 and
Core PRs #13/#14.

The first real WakeIntent has a lifetime limit of one. Consuming that slot without
recovery/arming observability would throw away a safety property that was explicitly
identified as P1 before `wake-prod-01`.

Therefore the next production transition is **not** a WakeIntent activation. It is a
provider-free, wake-off runtime upgrade to a current reviewed Agent/Core image that
contains `wake-status`.

## Contract

The gate:

1. starts from an active schema-v4 production service with WakeIntent disabled and
   refuses unless the running container still uses the exact frozen image above;
2. proves that frozen image is still present locally as rollback material;
3. records the current provider budget and the representative historical provider
   Task;
4. builds a clean, attributable candidate from the exact current Agent checkout and
   the exact `gaudere.ref` Core pin;
5. resolves the candidate once to a full immutable `sha256:` image ID and requires
   it to differ from the frozen production image;
6. refuses the candidate before service stop unless its control binary exposes
   `wake-status`;
7. stops the service and delegates the actual profile replacement to the existing
   `validate-schema-v4-service-wake-off.sh` gate;
8. that existing gate proves schema v4, zero wake rows, quiescent work, immutable
   image identity, no `--wake-intents`, zero provider effects, zero wake effects,
   stopped-state durability, restart safety and final active service;
9. after the candidate is running, the new `wake-status` command is invoked once and
   must fail with the specific `explicit wake capability is not enabled in this
   service` policy result. This simultaneously proves that the new observability
   surface exists and that WakeIntent is still disabled;
10. the provider budget and representative historical Task are checked again; and
11. the frozen previous image must still exist locally after success. Losing the
    rollback image makes the wrapper fail even if the candidate service is active.

The gate does **not** submit a Task, call OpenAI, consume a provider permit, accept or
revoke a WakeIntent, add `--wake-intents`, alter schema v4, open a port, or create a
successor action.

## Failure model

Before the service stop, failures leave production untouched.

After profile replacement begins, failure behavior is owned by the already-reviewed
schema-v4 wake-off validator: it attempts to stop the candidate, restores the exact
pre-gate Quadlet bytes and leaves the service stopped for human review. The wrapper
must not hide that state by blindly restarting anything.

A successful upgrade leaves the candidate service active with the same autostart
contract and WakeIntent disabled, while the exact previous immutable image remains
available for rollback.

## Separation from the first real wake

This upgrade makes `wake-prod-01` observable; it does not authorize it.

After this gate is successfully executed, the remaining first-wake sequence is:

1. obtain one separately authorized bounded reflection whose durable canonical
   result is `propose_wake` (this may consume provider permit #4);
2. freeze the exact source Task and production image identity;
3. separately authorize the one-time production WakeIntent activation/acceptance;
4. enable `--wake-intents`, prove `wake-status` starts empty and healthy, accept the
   exact source once, and prove duplicate idempotence;
5. restart before the persisted due time and prove exact durable re-arming;
6. let the host run without an open chat until the due time;
7. later observe one inert `fired` transition and prove that no provider Task,
   Action, provider consumption, external effect, polling loop or second wake was
   created.

That final sleep → autonomous host wake → later observation is the first experiment
that matters more than merely keeping a daemon alive.
