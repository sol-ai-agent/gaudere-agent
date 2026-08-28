# Rollback-safe production promotion runbook — current cognition candidate

Status: **plan only**. This document grants neither deployment authority nor provider-call authority.

Origin: issue #118, objective chosen by the first successful real repeatable
`cognition.current.v0` cycle (provider call #7).

The purpose of this runbook is to make one already-built, provenance-checked
candidate promotable without reconstructing intent from chat history. Executing
this document is a separate authority decision.

## 1. Frozen identities

These values are immutable inputs to this runbook.

```text
Candidate tag:
  localhost/gaudere-agent:current-cognition-c8b0999cf893
Candidate image ID:
  sha256:9dd20dbacee908e3a760080c4495a38991c208ffc81e90e48066156ec46072a9
Candidate Agent provenance:
  c8b0999cf8935ae60921b4031a0db927e3901c23
Candidate Core provenance:
  1316cf68db93e4c91a7bd79fbd289b8f382f8659

Current production image ID at plan creation:
  sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
Immediate rollback image ID:
  sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3
Older rollback image ID:
  sha256:6f2dab2ece7783556647f99204e4620a53b6574319310f5c61ffad8b579773d1
```

Provider call #7 is historical evidence, not permission for another call. It
produced exactly one confirmed provider Action, one succeeded current-cognition
Task, and moved durable provider usage from 6 to 7.

## 2. Files and variables

The promotion is allowed to change only the selected image reference in the
existing Gaudere Quadlet. All other service settings and all durable state must
remain unchanged.

```sh
set -eu

CANDIDATE_TAG='localhost/gaudere-agent:current-cognition-c8b0999cf893'
CANDIDATE_ID='sha256:9dd20dbacee908e3a760080c4495a38991c208ffc81e90e48066156ec46072a9'
CANDIDATE_AGENT='c8b0999cf8935ae60921b4031a0db927e3901c23'
CANDIDATE_CORE='1316cf68db93e4c91a7bd79fbd289b8f382f8659'

PRODUCTION_ID='sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01'
ROLLBACK_ID='sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3'
OLDER_ROLLBACK_ID='sha256:6f2dab2ece7783556647f99204e4620a53b6574319310f5c61ffad8b579773d1'

DB="$HOME/.local/share/gaudere/state/state.db"
QUADLET="$HOME/.config/containers/systemd/gaudere-agent.container"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
SAFE_DIR="$HOME/.local/share/gaudere/promotion-$STAMP"
mkdir -m 0700 "$SAFE_DIR"
```

Do not substitute another candidate, Core commit, production image or rollback
image while using this runbook. A different identity requires a new reviewed
plan.

## 3. Mandatory preflight — no mutation

All conditions below must pass before stopping production.

### 3.1 B10 and service

A fresh Sol B10 read-only request must succeed with exit code 0. B10 itself
must remain available; this runbook never stops the B10 executor.

```sh
systemctl --user is-active gaudere-agent.service
```

Expected: `active`.

The running container must still resolve to the frozen production image:

```sh
podman inspect gaudere-agent --format '{{.Image}}'
```

Expected exactly:

```text
sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01
```

If production already runs another image, **stop this procedure** and produce a
new plan. Do not attempt to "correct" drift during preflight.

### 3.2 Candidate provenance

```sh
test "$(podman image inspect --format '{{.Id}}' "$CANDIDATE_TAG")" = "$CANDIDATE_ID"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_AGENT"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_CORE"
```

All three protected historical images must still exist:

```sh
for image in "$PRODUCTION_ID" "$ROLLBACK_ID" "$OLDER_ROLLBACK_ID"; do
    podman image exists "$image"
done
```

Missing or mismatched identity is a hard stop.

### 3.3 Durable state and authority fences

```sh
test -f "$DB"
test "$(sqlite3 -readonly "$DB" 'PRAGMA user_version;')" = 4
```

The sole production WakeIntent must remain terminal and no deployed command
line may enable WakeIntent acceptance. Inspect both durable state and the
installed Quadlet/service definition. Any extra/ambiguous WakeIntent or any
unexpected `--wake-intents` capability is a hard stop.

Provider/current-cognition durable evidence must be unambiguous:

- provider lifetime usage is exactly 7 at the frozen starting point;
- the known call #7 current-cognition Task is terminal `succeeded`;
- exactly one confirmed provider Action exists for that Task;
- there is no pending/running/manual-review provider Action that could be
  mistaken for a fresh call;
- failed call #6 remains historical and is never retried.

A B10 preflight should print the relevant Task/Action/budget rows. If their
meaning is not unique, **stop**. Promotion must never be used to repair an
ambiguous provider effect.

## 4. Stopped-state checkpoint

Only after the read-only preflight passes:

```sh
systemctl --user stop gaudere-agent.service
test "$(systemctl --user is-active gaudere-agent.service || true)" = inactive
```

Freeze the exact service definition and durable DB while the owner process is
stopped:

```sh
cp --preserve=mode,timestamps "$QUADLET" "$SAFE_DIR/gaudere-agent.container.before"
sqlite3 "$DB" ".backup '$SAFE_DIR/state.before.db'"
sha256sum "$SAFE_DIR/gaudere-agent.container.before" > "$SAFE_DIR/gaudere-agent.container.before.sha256"
sha256sum "$SAFE_DIR/state.before.db" > "$SAFE_DIR/state.before.db.sha256"
```

The backup DB must pass integrity and schema checks:

```sh
test "$(sqlite3 -readonly "$SAFE_DIR/state.before.db" 'PRAGMA integrity_check;')" = ok
test "$(sqlite3 -readonly "$SAFE_DIR/state.before.db" 'PRAGMA user_version;')" = 4
```

Also freeze a logical durable-state fingerprint. This deliberately ignores
SQLite page/layout metadata and fingerprints the semantic database content:

```sh
sqlite3 -readonly "$DB" '.dump' | sha256sum > "$SAFE_DIR/state.logical.before.sha256"
```

Do not proceed unless the Quadlet backup, DB backup, their hashes and the
logical-state hash all exist.

## 5. Exact promotion boundary

The **only intended configuration mutation** is the value of the single
`Image=` line in the installed Gaudere Quadlet. No secret, network, command,
volume, capability, restart policy, WakeIntent flag or other line may change.

First verify that the current file contains exactly one image line:

```sh
test "$(grep -c '^Image=' "$QUADLET")" = 1
```

Create the promoted file from the stopped-state backup, replacing that one
line only:

```sh
python3 - "$SAFE_DIR/gaudere-agent.container.before" "$QUADLET" "$CANDIDATE_TAG" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
target = Path(sys.argv[2])
tag = sys.argv[3]
lines = source.splitlines(keepends=True)
indexes = [i for i, line in enumerate(lines) if line.startswith('Image=')]
if len(indexes) != 1:
    raise SystemExit('expected exactly one Image= line')
i = indexes[0]
ending = '\n' if lines[i].endswith('\n') else ''
lines[i] = f'Image={tag}{ending}'
target.write_text(''.join(lines))
PY
```

Review the mechanical diff:

```sh
diff -u "$SAFE_DIR/gaudere-agent.container.before" "$QUADLET" || true
```

The diff must contain exactly one removed `Image=...` line and exactly one
added `Image=$CANDIDATE_TAG` line. Any other change is a rollback trigger
before the service is started.

Then regenerate and start only the Gaudere service:

```sh
systemctl --user daemon-reload
systemctl --user start gaudere-agent.service
```

No provider command, current-cognition one-shot, wake command or other durable
mutation is part of this promotion.

## 6. Post-promotion acceptance

All checks are mandatory.

### 6.1 Service and image

```sh
test "$(systemctl --user is-active gaudere-agent.service)" = active
test "$(podman inspect gaudere-agent --format '{{.Image}}')" = "$CANDIDATE_ID"
```

A fresh image inspection must still report the frozen Agent/Core provenance.

### 6.2 DB and control/runtime health

```sh
test "$(sqlite3 -readonly "$DB" 'PRAGMA user_version;')" = 4
test "$(sqlite3 -readonly "$DB" 'PRAGMA integrity_check;')" = ok
```

The normal host helper must be able to reach the live control socket without
opening the DB through a second Agent process:

```sh
sh scripts/control-service.sh budget
```

This is observational and must not consume a permit.

### 6.3 No spontaneous durable effect

After the service is active and idle, recompute the logical DB fingerprint:

```sh
sqlite3 -readonly "$DB" '.dump' | sha256sum > "$SAFE_DIR/state.logical.after.sha256"
cmp "$SAFE_DIR/state.logical.before.sha256" "$SAFE_DIR/state.logical.after.sha256"
```

The fingerprints must be identical. In particular there must be:

- no new provider budget consumption;
- no new or changed provider Action;
- no new current-cognition Task;
- no changed WakeIntent;
- no hidden successor or wake effect.

Any semantic DB drift during promotion is a rollback trigger, not an invitation
to reinterpret the new state.

## 7. Rollback triggers

Rollback immediately if any of the following occurs:

1. service fails to become or remain `active`;
2. running image identity is not exactly `$CANDIDATE_ID`;
3. candidate Agent/Core provenance differs from the frozen values;
4. DB schema is not 4 or integrity check fails;
5. logical durable-state fingerprint changes during promotion;
6. control/runtime observational health fails;
7. any provider budget, Action, current-cognition Task or WakeIntent changes
   spontaneously;
8. the installed Quadlet differs from the stopped-state copy by more than the
   single intended `Image=` replacement;
9. any durable cognition evidence becomes noncanonical or ambiguous.

### Rollback procedure

```sh
systemctl --user stop gaudere-agent.service
cp --preserve=mode,timestamps \
    "$SAFE_DIR/gaudere-agent.container.before" "$QUADLET"
systemctl --user daemon-reload
```

If and only if the logical DB fingerprint changed, preserve the divergent DB
for postmortem, then restore the stopped-state backup:

```sh
if ! sqlite3 -readonly "$DB" '.dump' | sha256sum | \
     cmp - "$SAFE_DIR/state.logical.before.sha256"; then
    cp --preserve=mode,timestamps "$DB" "$SAFE_DIR/state.failed.db"
    sha256sum "$SAFE_DIR/state.failed.db" > "$SAFE_DIR/state.failed.db.sha256"
    rm -f "$DB-wal" "$DB-shm"
    cp --preserve=mode,timestamps "$SAFE_DIR/state.before.db" "$DB"
    restorecon -F "$DB" 2>/dev/null || true
fi

systemctl --user start gaudere-agent.service
test "$(systemctl --user is-active gaudere-agent.service)" = active
test "$(podman inspect gaudere-agent --format '{{.Image}}')" = "$PRODUCTION_ID"
```

Finally re-run schema/integrity, control health and durable-effect checks. If the
rollback service does not return to the frozen production image, stop further
mutation and escalate; do not try another image automatically.

## 8. Acceptance criteria for a later real cognition cycle

Promotion success does **not** authorize another provider call. A later,
separately authorized current-cognition cycle is accepted only if all of these
are observed:

1. a new immutable current-context snapshot is fresh (<=15 minutes) at claim
   and again at provider execution;
2. one explicit deterministic `cognition.current.v0` Task identity is frozen
   before provider execution;
3. durable budget preflight admits a new call;
4. exactly one provider Action corresponds to that Task;
5. durable provider usage increments by exactly one;
6. the Task becomes terminal with one canonical
   `gaudere.cognition.resume-decision.v1` `stop` or `continue` result;
7. a reopen/retry cannot invoke the same provider Action again;
8. no WakeIntent, shell, tool, successor or production authority is implied by
   the cognition result;
9. the normal service remains healthy after the bounded one-shot completes.

A transport effect followed by invalid cognition normalization is still a spent
provider call and is never replayed, as already proved by call #6.

## 9. Authority statement

This runbook is provider-free and non-deploying documentation. Its existence,
review or merge does **not** itself authorize:

- changing the production Quadlet;
- stopping/starting production for promotion;
- replacing an image;
- using the OpenAI secret;
- consuming provider call #8;
- acting automatically on a cognition proposal.

Those remain explicit later boundaries. The runbook only makes those boundaries
reproducible, attributable and rollback-safe.
