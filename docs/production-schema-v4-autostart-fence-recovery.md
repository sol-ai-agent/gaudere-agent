# Schema-v4 production autostart fence and catastrophic recovery

Status: **PREP ONLY / production not authorized**.

This contract closes the boot-time gap around the two directory renames used by
the schema-v3 to schema-v4 transaction. It does not authorize a Fedora run,
provider call, WakeIntent, merge, or production mutation.

## Why `systemctl disable` is not the fence

`gaudere-agent.service` is generated from the canonical Quadlet source:

```text
~/.config/containers/systemd/gaudere-agent.container
```

The source contains `[Install]` / `WantedBy=default.target`. Quadlet regenerates
the unit and its target relationship during `daemon-reload` and boot. Disabling
only the generated unit is therefore not a durable boot fence.

The transaction instead copies the exact original source into its durable
workspace, records its profile hash and prior systemd enablement observation,
stops the service, durably removes the canonical source, fsyncs its parent
directory, and runs `systemctl --user daemon-reload`. The state swap cannot begin
until the source is absent and the generated unit is not reported enabled.

## Transaction phases and boot behavior

| Durable phase/layout | Canonical profile | Canonical state | Boot behavior |
| --- | --- | --- | --- |
| Before `autostart-disarmed` | Exact rollback profile | Exact v3 | Prior behavior; v3 is still complete |
| `autostart-disarmed` through first rename | Absent | Exact v3 | Cannot auto-start |
| Rename gap | Absent | Absent; exact v3 is `state.pre-v4-*` | Cannot auto-start or recreate the bind source |
| v4 prepared/live probes | Candidate profile without `[Install]` or `WantedBy` | Validated v4; exact v3 retained | Manual starts only; cannot auto-start |
| Durable profile commit | Candidate profile with the prior autostart contract | Validated v4; exact v3 retained | Safe to preserve the prior boot behavior |

The enabled candidate source is installed only after the v4 state, immutable
candidate image, provider budget, representative Task, disabled WakeIntent
capability, and active candidate process have all passed their final audit. The
durable source replacement is the commit point. A power loss after that
replacement is safe because both the canonical state and the boot profile are the
validated candidate pair.

If the original profile was active but intentionally lacked `WantedBy`, success
keeps the candidate profile disarmed while leaving the explicitly started service
active.

## Durable recovery evidence

Each transaction workspace under
`STATE_PARENT/.schema-v4-transitions/transition.*` is retained. It contains:

- `phase`: last fsynced phase;
- `profile.before`: exact mode-0600 rollback Quadlet source;
- `profile.before.sha256`: recorded checksum;
- `profile.autostart.before`: `enabled` or `disarmed` source contract;
- `service.enablement.before`: exact preflight `systemctl is-enabled` observation;
- `autostart.fence`: `DISARMED` once the boot fence is durable;
- `v3.before.json`: complete logical v3 snapshot once `snapshot_ready` was reached;
- captured deployment and wake-off outputs when those phases ran.

Never infer a path from the newest timestamp alone. Select one exact workspace,
inspect its `phase` and retained outputs, and reconcile it with the actual state
layout before moving anything.

## Catastrophic power-loss recovery

This is an operator procedure for a disposable review first. It is **not** a GO
for production. Replace every example value with one inspected absolute path; do
not run it with empty variables, globs, or guessed timestamps.

1. Disarm first, even if the profile appears already absent:

   ```sh
   systemctl --user stop gaudere-agent.service || true
   test "$(systemctl --user is-active gaudere-agent.service 2>/dev/null || true)" = inactive

   PROFILE=$HOME/.config/containers/systemd/gaudere-agent.container
   if test -e "$PROFILE"; then
       test ! -e "${PROFILE}.manual-review"
       mv -- "$PROFILE" "${PROFILE}.manual-review"
   fi
   systemctl --user daemon-reload
   test "$(systemctl --user is-active gaudere-agent.service 2>/dev/null || true)" = inactive
   ```

   If the service cannot be proved inactive, stop. Do not rename state trees.

2. Inspect one exact transaction and layout:

   ```sh
   WORKSPACE=/absolute/inspected/.schema-v4-transitions/transition.EXACT
   STATE_PARENT=/absolute/inspected/gaudere
   STATE_DIR=$STATE_PARENT/state
   ROLLBACK_DIR=$STATE_PARENT/state.pre-v4-EXACT

   test -f "$WORKSPACE/profile.before"
   sha256sum -c "$WORKSPACE/profile.before.sha256"
   sed -n '1p' "$WORKSPACE/phase"
   find "$STATE_PARENT" -maxdepth 1 -mindepth 1 -printf '%f\n' | sort
   ```

3. For the first-rename gap, require the canonical path to be absent and the exact
   rollback tree to be schema v3 before restoring it:

   ```sh
   test ! -e "$STATE_DIR"
   test -f "$ROLLBACK_DIR/state.db"
   python3 - "$ROLLBACK_DIR/state.db" <<'PY'
   import sqlite3
   import sys
   import urllib.parse

   uri = "file:" + urllib.parse.quote(sys.argv[1]) + "?mode=ro"
   with sqlite3.connect(uri, uri=True) as db:
       assert db.execute("PRAGMA user_version").fetchone()[0] == 3
       assert [row[0] for row in db.execute("PRAGMA integrity_check")] == ["ok"]
   PY
   mv -- "$ROLLBACK_DIR" "$STATE_DIR"
   ```

   If both `state` and `state.pre-v4-*` exist, or neither exists, stop for manual
   review. Do not overwrite, merge, copy into, or delete either tree.

4. If canonical `state` is a failed v4 tree, first prove service inactivity and
   acquire its exact `state.db.lock`. Move that complete tree to a new, explicitly
   checked unused `state.failed-v4-*` path, release the lock, and only then rename
   the exact v3 rollback tree to `state`. Any lock refusal or path collision is a
   hard stop.

5. Validate restored v3 against `v3.before.json` using the transaction validator.
   Keep autostart disarmed if any byte, schema, budget, Task, image, or path check
   is uncertain.

6. Only after exact v3 validation, restore the rollback profile and reload it:

   ```sh
   install -m 0600 "$WORKSPACE/profile.before" "$PROFILE"
   systemctl --user daemon-reload
   cmp "$WORKSPACE/profile.before" "$PROFILE"
   systemctl --user is-enabled gaudere-agent.service || true
   ```

   Compare the final enablement observation with
   `service.enablement.before`. Confirm that the profile's `Image=` resolves to the
   retained full rollback image ID. Only then may an explicitly authorized
   operator start the service and verify it is active on exact schema v3.

For every post-swap ordinary failure handled by the script, recovery restores the
exact v3 tree but deliberately leaves the canonical Quadlet source absent, the
service inactive, and the rollback profile retained in the workspace. This is the
default `manual_review` posture; there is no blind restart.

## Synthetic proof

`tests/production_schema_v4_transaction_test.sh` covers:

- success with prior autostart enabled and disarmed;
- pre-swap failure with exact enablement/profile restoration;
- post-swap and inner-stage rollback with source absent and service inactive;
- a hard SIGKILL immediately after `state -> state.pre-v4-test`;
- a simulated reboot at that rename gap proving no service start;
- the ordered disposable recovery: profile absent, exact v3 restored, rollback
  profile restored, then explicit service start.

The installer and wake-off gate tests separately prove that the candidate profile
can be rendered without `[Install]` / `WantedBy`, used for explicit live probes,
and removed again on a failed uncommitted gate.
