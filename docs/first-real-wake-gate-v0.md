# First real WakeIntent gate v0

Status: **PREP ONLY**. Merging and CI do not enable WakeIntent, accept a wake, or invoke a provider.

## Experimental source

The source is frozen from the real fourth permanent provider call:

- Task: `production-reflection-wake-source-first`
- runtime image: `sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01`
- runtime Agent: `4e6cb09467456f38377bd8610e1ac534c7705380`
- Core: `1316cf68db93e4c91a7bd79fbd289b8f382f8659`
- provider budget after source creation: 4/12
- decision: `propose_wake`
- delay: 3600 seconds
- exact reason: `Resume after a one-hour production observation window to verify that the active pre-wake runtime leaves durable, interpretable evidence, journal the result, and identify the single reliability condition that should gate any future WakeIntent enablement. This advances cooperation reliability without spending scarce provider budget on a more ambitious step.`

The source is evidence. This gate must not regenerate, rewrite, shorten, or replace it.

## Why two phases

The wake delay starts when the proposal is explicitly accepted, not when the provider produced it.

**Phase A** performs the authority transition and restart proof before the deadline:

1. prove exact runtime/source/provider state and WakeIntent OFF;
2. stop the service and hold the production state lock;
3. prove schema 4, zero wake rows, zero nonterminal Tasks/Actions, provider count 4, and snapshot all non-wake durable state;
4. preserve the exact installed wake-OFF Quadlet;
5. create a wake-enabled Quadlet by adding only `--wake-intents` to its sole `Exec=` line;
6. start and prove the fixed wake scope is empty/healthy;
7. issue exactly one first acceptance for the frozen source;
8. issue one same-source duplicate acceptance only to prove idempotency and unchanged deadline;
9. prove `due_at_ms - accepted_at_ms = 3,600,000`;
10. restart before due and prove the same durable deadline is re-armed;
11. stop briefly under the state lock and prove every non-wake durable table is unchanged;
12. restart and leave the service active with the one wake scheduled.

A pre-acceptance failure restores the exact wake-OFF profile. Once acceptance may have committed, an error is different: the lifetime slot cannot be rolled back. The gate therefore preserves all proof material and leaves the service stopped for manual review rather than pretending that recovery erased acceptance.

**Phase B** is deliberately a separate command and never sleeps internally. If run before `due_at_ms`, it refuses without changing state. After due it:

1. requires the exact Phase-A proof/profile and runtime;
2. observes the unique record as `fired`, with `terminal_at_ms >= due_at_ms`;
3. proves provider total is still 4;
4. stops and locks the database;
5. proves exactly one fired wake row and no nonterminal Task/Action;
6. compares all non-wake durable state byte-for-byte (canonical JSON) with the Phase-A baseline;
7. restores the exact original wake-OFF Quadlet;
8. starts the service and proves WakeIntent is disabled again and provider total remains 4.

Firing is intentionally inert: there is no successor Task, provider invocation, callback, shell, network action, or second wake.

## Production authorization boundaries

Phase A requires a later explicit Sol + Bertrand authorization and the exact command argument:

```sh
sh scripts/run-first-real-wake-phase-a-v0.sh --execute-after-explicit-first-wake-go
```

That authorization covers only enabling the already-reviewed inert capability and accepting the exact frozen source once (plus one idempotency replay). It does **not** authorize another provider call or any successor cognition.

Phase B is observation and closeout after the persisted deadline:

```sh
sh scripts/run-first-real-wake-phase-b-v0.sh --observe-after-due-and-close
```

Expected Phase-A terminal proof:

```text
deadline_delta_ms=3600000
duplicate_deadline_identity=PASS
restart_rearm=PASS
nonwake_state_unchanged=PASS
provider_effects=0
wake_acceptance_effects=1
wake_status=scheduled
wake_capability_active=true
service_final=active
gaudere first real wake phase A: PASS
```

Expected Phase-B terminal proof:

```text
wake_terminal=fired
nonwake_state_unchanged=PASS
provider_effects=0
successor_effects=0
wake_capability_active=false
service_final=active
provider_total_after=4
gaudere first real wake phase B: PASS
```

## Audit material

Phase A creates a private proof directory at:

`~/.local/share/gaudere/wake-proof-v0/first-real-wake/`

It retains the exact wake-OFF and wake-enabled profiles, source/budget reports, accepted and duplicate reports, pre/post-restart status, canonical non-wake snapshots, hashes and accepted/due timestamps. Phase B adds terminal status, final snapshot, lateness, restored wake-OFF proof and closeout metadata. These records are part of the experiment and should not be cleaned as ordinary temporary files.
