# Schema v4 service profile with WakeIntent disabled

Status: **PREP ONLY / NOT AUTHORIZED FOR PRODUCTION**.

This gate is the transition immediately after a separately authorized and successful
production schema-v3 -> v4 migration. It proves that the ordinary OpenAI-capable
`gaudere-agent.service` can reopen the v4 production state while explicit WakeIntent
remains dormant. It does not authorize a WakeIntent, a provider Task, or a fourth
provider call.

## Separation of gates

Schema v4 and WakeIntent activation are independent. The reviewed OpenAI Quadlet
contains no `--wake-intents`; without that flag the service constructs no
`WakeIntentStore`, `WakeIntentRuntime`, or `ExplicitWake`. Existing live control then
answers an observational `wake ID` request with
`explicit wake capability is not enabled in this service` before touching wake state.

The later first-WakeIntent gate remains separate and blocked. Issue #60 also owns future
read-only recovery/arming observability; this gate deliberately does not add a runtime
status API merely to prove dormancy.

## Validator

`scripts/validate-schema-v4-service-wake-off.sh REPRESENTATIVE_PROVIDER_TASK_ID`
encodes the P7/P8/P9 proof described by the production-v4 deployment design.

A real run requires all of:

- a separately authorized production-v4 migration already completed;
- service initially exactly `inactive`;
- explicit `GAUDERE_STATE_DIR` and `GAUDERE_SERVICE_NAME`;
- the reviewed runtime already reachable through `localhost/gaudere-agent:dev`;
- `GAUDERE_EXPECTED_RUNTIME_IMAGE_ID` equal to the candidate image ID approved by the
  preceding provenance/migration gate;
- `GAUDERE_SCHEMA_V4_WAKE_OFF_AUTHORIZATION=AUTHORIZED_SCHEMA_V4_WAKE_OFF_GATE`.

The authorization token is intentionally absent from CI and from normal service startup.
It is not a standing production permission.

## Proof sequence

1. While stopped and under `state.db.lock`, require schema v4, SQLite integrity, the
   exact dormant wake schema, zero wake rows, no nonterminal Task/Action, and a succeeded
   representative provider Task with durable usage metadata.
2. Snapshot every non-wake SQLite object and row, including all provider budget
   consumptions.
3. Fence the mutable runtime tag against the expected immutable image ID.
4. Run the already reviewed OpenAI service installer. Require the installed Quadlet to
   be byte-identical to the source template and contain no `--wake-intents`.
5. Start the service and perform **observations only**: budget, representative Task, and
   a `wake` lookup that must fail because wake is disabled. No `echo`, `openai`,
   `reflect`, `accept-wake`, or `revoke-wake` is issued.
6. Require no `explicit wake enabled` log.
7. Stop normally and require `gaudere-agent: safe`.
8. Under the flock, require the exact non-wake durable snapshot to match the pre-start
   snapshot and wake rows to remain zero.
9. Restart once more, repeat the owner-mediated observations, recheck image/profile
   identity, and leave the service active.

The provider budget is compared by durable consumption count. Clock-derived fields such
as rolling-window use or cooldown are intentionally not frozen across the stop/restart.

## Failure behavior

If any step after profile replacement fails, the validator tries to stop the service,
restores the exact pre-gate Quadlet bytes, reloads systemd, and leaves the service stopped
for review. It never changes the schema back to v3: the v3 rollback tree, backup, and
prior image from the separately authorized migration gate remain the recovery artifacts
for a schema rollback.

If the service cannot be proven inactive during failure recovery, the validator does not
claim the prior profile was restored and reports the observed state for human recovery.

## CI proof

`tests/schema_v4_wake_off_service_gate_test.sh` uses only a temporary SQLite fixture and
fake `systemctl`, `journalctl`, Podman, installer, and live-control commands. It proves:

- the success path leaves the synthetic service active with an unchanged v4 durable
  state, zero wake rows, and the reviewed wake-off profile;
- only `budget`, `task`, and observational `wake` live-control operations are used;
- a pre-existing wake row fails before service startup;
- an unexpectedly enabled wake command or an `explicit wake enabled` log is fatal;
- post-start failures restore the prior profile and leave the service stopped.

CI mounts no production state or secret and performs no OpenAI call.
