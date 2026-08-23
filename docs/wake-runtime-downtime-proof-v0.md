# WakeIntent runtime-downtime proof v0

## Purpose

Prove the remaining WakeIntent reliability property without powering off, rebooting, logging out, or otherwise disrupting the Fedora workstation.

The property under test is narrower and directly relevant to WakeIntent durability:

> If the isolated Gaudere runtime is absent across `due_at`, the same durable SQLite WakeIntent must reconcile exactly once to `fired` when that runtime next starts, with positive lateness and no provider, successor, duplicate, or non-wake effect.

## Safety boundary

This proof may start, restart, stop, and remove only the isolated staging service `gaudere-wake-staging.service` and its staging state/profile.

It MUST NOT:

- power off or reboot Fedora;
- log out or terminate the user session;
- stop or restart `gaudere-agent.service` production;
- enable production WakeIntent;
- submit provider work;
- create a successor task;
- use network access from staging.

The staging Quadlet remains `Network=none`, no-secret, with a state DB separate from production.

## Existing armed fixture

The original host-downtime ARM remains valid as the isolated fixture. Its `phase-arm.meta` supplies immutable source, accepted time and due time. A previously attempted host poweroff was blocked by the desktop session. Any resulting `poweroff.meta` is historical evidence of that failed approach and is not an input to this proof.

## Phase 1: stopped-runtime witness

Before `due_at`, with staging already inactive, run:

```sh
sh scripts/run-wake-runtime-downtime-stop-v0.sh --record-staging-stop-before-due
```

The gate requires:

- production active;
- production WakeIntent off;
- provider total still 4;
- same Fedora boot as ARM;
- staging inactive;
- exactly one still-scheduled staging WakeIntent row with the frozen source and deadline;
- current time strictly before `due_at`.

It writes `runtime-stop.meta` and performs no host action.

## Phase 2: overdue reconciliation

Leave staging inactive across `due_at`. After the deadline, run:

```sh
sh scripts/run-wake-runtime-downtime-observe-v0.sh --observe-staging-after-due-and-close
```

Before starting staging, the gate proves directly from SQLite that the row is still scheduled and terminal-free after the deadline. It also requires the same Fedora boot as ARM, proving this is runtime/container downtime rather than a host reboot.

It then starts only staging and requires:

- transition to `fired`;
- `terminal_at_ms > due_at_ms`;
- positive lateness;
- repeated observation leaves terminal state unchanged;
- one controlled staging restart leaves terminal state unchanged;
- staging non-wake durable state unchanged;
- production active, frozen, WakeIntent off and provider total 4;
- production durable baseline unchanged.

Finally it stops staging, saves the final staging DB into the proof bundle, removes the staging Quadlet/state, and records `phase-runtime-observe.meta`.

## PASS meaning

A PASS proves runtime absence across the deadline and exact overdue reconciliation on the same host boot. It does not claim proof of Fedora shutdown/reboot or Quadlet boot-time autostart. Those are separate host-management properties and are intentionally out of scope for this WakeIntent durability experiment.