# WakeIntent host-downtime proof v0

Status: **PREP ONLY**. Issue: #81.

This gate closes the single host-level reliability condition retained after the first real production WakeIntent completed successfully: prove that an already-durable WakeIntent whose deadline passes while Fedora is unavailable is reconciled exactly once on the first later safe boot, with positive lateness evidence and no hidden work.

## Why staging is mandatory

The production WakeIntent v0 scope has a lifetime maximum of one accepted row per database. The first real production wake consumed that slot permanently and finished `fired`; it must not be deleted, recycled, or replaced.

The host-downtime proof therefore uses the same immutable runtime image but a distinct rootless staging service and database. Production remains active, WakeIntent OFF, provider total 4/12.

Staging boundaries:

- image: frozen production image `sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01`;
- container/service: `gaudere-wake-staging` / `gaudere-wake-staging.service`;
- state: `~/.local/share/gaudere/wake-host-downtime-v0/state/state.db`;
- control socket: `/tmp/gaudere-wake-staging-control.sock`;
- `Network=none`;
- no OpenAI arguments, secret, provider submission path, published port, or inbound listener;
- `--wake-intents` exists only in the staging Quadlet;
- `[Install] WantedBy=default.target` allows the user manager to bring staging back after reboot.

## Staging fixture

`run-wake-host-downtime-arm-v0.sh` first takes a consistent SQLite backup of live production. It proves the logical clone equals the captured production snapshot. The copied database necessarily contains the permanently consumed production wake row.

Only in the isolated copy, the script temporarily drops the `wake_intents` triggers, deletes the copied wake row, and recreates the exact trigger SQL in the same transaction. It then proves:

- schema v4 and `integrity_check=ok`;
- full schema object set unchanged;
- empty staging wake scope;
- every non-wake table/object is logically identical to the production snapshot;
- the exact canonical source Task `production-reflection-wake-source-first` remains present and unchanged.

The production database is never targeted by this reset. A full logical production snapshot is compared again after staging acceptance.

## Three separately bounded operations

### 1. Arm isolated staging

Future explicit command:

```sh
sh scripts/run-wake-host-downtime-arm-v0.sh --prepare-after-explicit-host-downtime-go
```

The script starts staging, requires an empty fixed wake scope, accepts exactly the canonical source, performs one same-source duplicate acceptance to prove deadline identity, and leaves the staged wake `scheduled` for its original 3600-second delay.

It records `phase-arm.meta` with boot ID, `accepted_at_ms`, `due_at_ms`, runtime provenance and staging profile hash.

### 2. Request real poweroff before due

This is a separate destructive host gate and requires a separate explicit authorization:

```sh
sh scripts/run-wake-host-downtime-poweroff-v0.sh --poweroff-after-explicit-host-downtime-go
```

The script refuses to run unless at least two minutes remain before due, production is healthy/WakeIntent OFF/provider total 4, and staging is still the exact scheduled wake. It durably records the current boot ID and pre-due poweroff request time, then runs `systemctl poweroff`.

The host must remain off until after `due_at_ms`.

### 3. Observe only after booting past due

After Fedora is booted only after the deadline, run:

```sh
sh scripts/run-wake-host-downtime-observe-v0.sh --observe-after-reboot-and-close
```

The observer intentionally does **not** start staging. Before any staging mutation it requires:

- a different boot ID from the arm/poweroff boot;
- current `/proc/stat` boot start strictly after `due_at_ms`;
- staging service already active, proving return through user-manager/Quadlet startup;
- exact staging profile hash and frozen image.

It then requires one `fired` row with the original identity/timestamps, `terminal_at_ms >= due_at_ms`, and `lateness_ms > 0`. A second observation and one controlled restart must preserve the exact terminal timestamp. Non-wake staging state must equal the pre-acceptance baseline.

Production must still match its original logical snapshot, remain active with WakeIntent OFF, and retain provider total 4.

On PASS, staging is stopped, a consistent final staging proof DB is copied into the proof bundle, the staging Quadlet/state directory are removed, and production remains untouched.

## Failure semantics

Before staged acceptance, failure removes the staging profile/state/proof and leaves production unchanged.

Once staged acceptance may have committed, failure stops staging but preserves its profile, database and proof directory for manual review. The lifetime slot in that staging DB is never silently recreated or retried.

After reboot, any missing autostart, ambiguous boot evidence, changed terminal timestamp, non-wake drift, provider drift, production drift, or cleanup ambiguity is a failure—not a fabricated PASS.

## Expected final evidence

A successful real run must include:

- `host_down_across_deadline=PASS`;
- `staging_autostart_after_reboot=PASS`;
- positive `lateness_ms`;
- `single_terminal_transition=PASS`;
- `nonwake_state_unchanged=PASS`;
- `provider_effects=0`;
- `successor_effects=0`;
- `production_untouched=PASS`;
- `production_wake_capability_active=false`;
- `production_provider_total_after=4`;
- `staging_profile_removed=PASS`;
- production service final `active`.

PREP/CI/merge do not authorize any real staging acceptance, host poweroff, reboot, provider call, or production WakeIntent activation.
