# Rollback-safe production promotion runbook — current cognition candidate

Status: **plan only**. This document grants neither deployment authority nor provider-call authority.

Origin: issue #118, objective chosen by the first successful real repeatable
`cognition.current.v0` cycle (provider call #7).

The purpose of this runbook is to make one already-built, provenance-checked
candidate promotable without reconstructing intent from chat history. Executing
this document is a separate authority decision.

## 1. Frozen identities

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

## 2. Variables

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
```

Do not substitute another candidate, Core commit, production image or rollback
image while using this runbook. A different identity requires a new reviewed
plan.

## 3. Mandatory read-only preflight

All conditions must pass before production is stopped. A fresh Sol B10
read-only request must succeed with exit code 0; this runbook never stops B10.

### 3.1 Service, installed image reference and running image

```sh
test "$(systemctl --user is-active gaudere-agent.service)" = active
test "$(grep -c '^Image=' "$QUADLET")" = 1
CURRENT_IMAGE_REF="$(sed -n 's/^Image=//p' "$QUADLET")"
test -n "$CURRENT_IMAGE_REF"
test "$(podman image inspect --format '{{.Id}}' "$CURRENT_IMAGE_REF")" = "$PRODUCTION_ID"
test "$(podman inspect gaudere-agent --format '{{.Image}}')" = "$PRODUCTION_ID"
```

The installed rollback reference and the actually running image must both still
resolve to the frozen production image. If either has drifted, stop and produce
a new plan; do not repair drift during promotion.

### 3.2 Candidate and protected image provenance

```sh
test "$(podman image inspect --format '{{.Id}}' "$CANDIDATE_TAG")" = "$CANDIDATE_ID"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_AGENT"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_CORE"

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

A B10 read-only snapshot must additionally establish all of the following:

- the sole production WakeIntent remains terminal `fired`;
- neither the installed Quadlet nor running command line enables unexpected
  WakeIntent acceptance;
- provider lifetime usage is exactly 7 at this frozen starting point;
- the call #7 `cognition.current.v0` Task is terminal `succeeded`;
- exactly one confirmed provider Action exists for that Task;
- no pending/running/manual-review provider Action can be confused with a new
  call;
- failed call #6 remains historical and is never retried.

Any ambiguous provider, cognition or WakeIntent evidence is a hard stop.
Promotion must not be used to repair ambiguous effects.

## 4. Stopped-state checkpoint

Only after the read-only preflight passes:

```sh
systemctl --user stop gaudere-agent.service
test "$(systemctl --user is-active gaudere-agent.service || true)" = inactive

mkdir -m 0700 "$SAFE_DIR"
printf '%s\n' "$CURRENT_IMAGE_REF" > "$SAFE_DIR/image-ref.before"
cp --preserve=mode,timestamps "$QUADLET" "$SAFE_DIR/gaudere-agent.container.before"
sqlite3 "$DB" ".backup '$SAFE_DIR/state.before.db'"
sha256sum "$SAFE_DIR/gaudere-agent.container.before" > "$SAFE_DIR/gaudere-agent.container.before.sha256"
sha256sum "$SAFE_DIR/state.before.db" > "$SAFE_DIR/state.before.db.sha256"

test "$(sqlite3 -readonly "$SAFE_DIR/state.before.db" 'PRAGMA integrity_check;')" = ok
test "$(sqlite3 -readonly "$SAFE_DIR/state.before.db" 'PRAGMA user_version;')" = 4
sqlite3 -readonly "$DB" '.dump' | sha256sum > "$SAFE_DIR/state.logical.before.sha256"
```

The backup DB hash is the stopped-state rollback proof. The logical dump hash
fingerprints semantic durable state independently of SQLite page layout.

## 5. Exact promotion boundary

The **only intended configuration mutation** is the value of the single
`Image=` line in the installed Gaudere Quadlet. Secret, network, Exec, volume,
capability, restart policy, WakeIntent flags and every other line must remain
byte-for-byte identical to the stopped-state copy.

Create the promoted file from that copy:

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

diff -u "$SAFE_DIR/gaudere-agent.container.before" "$QUADLET" || true
```

The diff must contain exactly one removed `Image=...` line and one added
`Image=$CANDIDATE_TAG` line. Any other change is a rollback trigger before
startup.

Then:

```sh
systemctl --user daemon-reload
systemctl --user start gaudere-agent.service
```

No provider command, cognition one-shot, WakeIntent command or other durable
mutation belongs to promotion.

## 6. Post-promotion acceptance

### 6.1 Service and exact image

```sh
test "$(systemctl --user is-active gaudere-agent.service)" = active
test "$(podman inspect gaudere-agent --format '{{.Image}}')" = "$CANDIDATE_ID"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.agent.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_AGENT"
test "$(podman image inspect --format '{{index .Labels "io.gaudere.core.revision"}}' "$CANDIDATE_TAG")" = "$CANDIDATE_CORE"
```

### 6.2 DB and live control health

```sh
test "$(sqlite3 -readonly "$DB" 'PRAGMA user_version;')" = 4
test "$(sqlite3 -readonly "$DB" 'PRAGMA integrity_check;')" = ok
sh scripts/control-service.sh budget
```

`budget` is observational and must not consume a provider permit.

### 6.3 No spontaneous durable effect

```sh
sqlite3 -readonly "$DB" '.dump' | sha256sum > "$SAFE_DIR/state.logical.after.sha256"
cmp "$SAFE_DIR/state.logical.before.sha256" "$SAFE_DIR/state.logical.after.sha256"
```

The logical fingerprints must be identical. Therefore promotion itself caused:

- no provider budget consumption;
- no new or changed provider Action;
- no new current-cognition Task;
- no changed WakeIntent;
- no hidden successor or wake effect.

Any semantic DB drift is a rollback trigger, not an invitation to reinterpret
the new state.

## 7. Rollback triggers and procedure

Rollback immediately if any of these occurs:

1. service fails to become or remain `active`;
2. running image is not exactly `$CANDIDATE_ID`;
3. Agent/Core provenance differs from the frozen candidate;
4. DB schema is not 4 or integrity check fails;
5. logical durable-state fingerprint changes;
6. live control health fails;
7. any provider budget, Action, current-cognition Task or WakeIntent changes
   spontaneously;
8. the Quadlet changed by more than its single `Image=` line;
9. durable cognition evidence becomes noncanonical or ambiguous.

Rollback configuration first:

```sh
systemctl --user stop gaudere-agent.service
cp --preserve=mode,timestamps \
    "$SAFE_DIR/gaudere-agent.container.before" "$QUADLET"
systemctl --user daemon-reload
```

If and only if the logical DB fingerprint changed, preserve the divergent DB
and restore the stopped-state backup. Do **not** run a generic `restorecon` on
the DB: the Podman `:Z` volume owns the SELinux labeling policy.

```sh
if ! sqlite3 -readonly "$DB" '.dump' | sha256sum | \
     cmp - "$SAFE_DIR/state.logical.before.sha256"; then
    cp --preserve=mode,timestamps "$DB" "$SAFE_DIR/state.failed.db"
    sha256sum "$SAFE_DIR/state.failed.db" > "$SAFE_DIR/state.failed.db.sha256"
    rm -f "$DB-wal" "$DB-shm"
    cp --preserve=mode,timestamps "$SAFE_DIR/state.before.db" "$DB"
fi

systemctl --user start gaudere-agent.service
test "$(systemctl --user is-active gaudere-agent.service)" = active
test "$(podman inspect gaudere-agent --format '{{.Image}}')" = "$PRODUCTION_ID"
test "$(sqlite3 -readonly "$DB" 'PRAGMA user_version;')" = 4
test "$(sqlite3 -readonly "$DB" 'PRAGMA integrity_check;')" = ok
```

Re-run control health and durable-effect checks. If rollback does not return to
the frozen production image, stop further mutation and escalate; do not try a
second fallback image automatically. The two older rollback images remain
protected evidence/options but are not automatically selected by this runbook.

## 8. Acceptance criteria for a later real cognition cycle

Promotion success does **not** itself authorize another provider call. A later
current-cognition cycle is accepted only when:

1. a new immutable current-context snapshot is fresh (<=15 minutes) at claim
   and again at provider execution;
2. one deterministic `cognition.current.v0` Task identity is frozen before the
   provider boundary;
3. durable budget preflight admits a new call;
4. exactly one provider Action corresponds to that Task;
5. durable provider usage increments by exactly one;
6. the Task becomes terminal with one canonical
   `gaudere.cognition.resume-decision.v1` `stop` or `continue` result;
7. reopen/retry cannot invoke the same provider Action again;
8. no WakeIntent, shell, tool, successor or production authority is implied by
   the cognition result;
9. the normal service remains healthy after the bounded one-shot completes.

A transport effect followed by invalid cognition normalization is still a spent
provider call and is never replayed, as call #6 already proved.

## 9. Authority statement

This runbook is provider-free and non-deploying documentation. Its existence,
review or merge does **not** itself authorize:

- changing the production Quadlet;
- promoting an image;
- using the OpenAI secret;
- consuming provider call #8;
- acting automatically on a cognition proposal.

Those remain later bounded actions. The runbook only makes their production
boundary reproducible, attributable and rollback-safe.
