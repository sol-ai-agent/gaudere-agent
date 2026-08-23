# Production SQLite schema v4 deployment gate

Status: design only; no production command is authorized by this document.

Work packet: `GAUDERE-WORKERMAX-PROD-SCHEMA-V4-DESIGN-20260822`

Agent design base: `e24d2bbfdb590e0caa82044b7af6912adfb29775`

Pinned Core on that base: `c24c40b84a12e51515cee4611e3dc79e9fd83892`

## Decision

The production v3 to v4 transition should be a stopped-state, same-filesystem
directory swap from a fully validated restore of a fresh backup. The original v3
state tree becomes the rollback tree; it is never migrated in place. The installed
OpenAI Quadlet remains byte-for-byte unchanged and continues to omit
`--wake-intents`.

The gate is **not ready to execute yet**. Two prerequisites must be resolved first:

1. The image recorded in the last production deployment was built from Agent
   `5094dee1a0a182c2e9a212d72f2c1cdbac08ac0b`, which pins Core
   `74a801d4d0cc2bc229e25b33c0174ce54b683ab2`. Both stores in that Core reject
   `PRAGMA user_version > 3`. That image cannot reopen v4. A current, exactly pinned
   image must first replace the mutable `localhost/gaudere-agent:dev` tag and be
   proven against the untouched production v3 database. The prior `:dev` image ID
   must be captured and retained under a verified immutable rollback tag before any
   candidate build command that could mutate `:dev`.
2. `scripts/install-openai-user-service.sh` currently accepts only schema v3. The
   migration gate does not reinstall or edit the already installed profile, but a
   post-migration reinstall/recovery path would be blocked. Before production v4 is
   declared operational, a separate reviewed change must let the installer accept
   exactly v3 or v4 without changing the rendered Quadlet or adding
   `--wake-intents`.

These are runtime/recovery prerequisites, not reasons to alter the service profile
or enable the wake capability.

## Non-goals

This gate does not accept, revoke, or fire a wake; create a Task; invoke a provider;
change provider budgets; install a secret; change a Quadlet; enable
`--wake-intents`; add a port; or merge the later exact-wake activation gate.

Before any production-v4 execution, a provider-free real-Fedora disposable-copy
proof on a fresh real backup must be observed and recorded in authoritative
continuity. PR #47 proves the migration path with CI/disposable fixtures; that is
not host evidence. Once continuity records the real-host gate as PASS, the proof
must not be repeated without a specific new reason. This design consumes the gate
status recorded at execution time and adds the production swap, runtime-image,
live-reopen, and automatic rollback requirements.

## Names and durable artifacts

All state directories used by the swap must have the same parent filesystem.
Names below are illustrative; the implementation must print the resolved absolute
paths before the first mutation.

| Name | Purpose | Lifetime |
| --- | --- | --- |
| `state` | Installed state tree | Active tree only |
| `.state.v4-work-<stamp>` | Restore and migration staging tree | Removed only before swap or retained on failure |
| `state.pre-v4-<stamp>` | Exact original production v3 tree | Retained after success |
| `state.failed-v4-<stamp>` | Failed installed/staging v4 tree | Retained for diagnosis |
| `gaudere-state-<stamp>.tar.gz` and `.sha256` | Fresh, flock-protected v3 backup | Immutable |
| `rollback-before-schema-v4-<stamp>` | Prior production image ID under a local tag | Retained with the v3 tree |

No pre-v3 rollback directory or earlier backup may be renamed, overwritten, or
deleted by this gate.

## State machine

| State | Meaning | Permitted next state |
| --- | --- | --- |
| `P0 stopped-v3` | Service inactive; original v3 is locked and validated | `P1`, `BLOCKED` |
| `P1 candidate-on-v3` | Current image has opened the unchanged v3 through the unchanged profile, then stopped safely | `P2`, `ROLLBACK-IMAGE` |
| `P2 backup-verified` | Fresh archive and checksum independently restore as v3 | `P3`, `BLOCKED` |
| `P3 staging-v4` | Only the restored copy was migrated with `--check --wake-intents` | `P4`, `BLOCKED` |
| `P4 staging-proven` | v4 shape, zero wake rows, default reopen, snapshots, and budget are proven | `P5`, `BLOCKED` |
| `P5 swap-fenced` | Source v3, image ID, profile hash, lock, and service state were rechecked | `P6`, `BLOCKED` |
| `P6 installed-v4` | Directory swap completed; service remains stopped | `P7`, `ROLLBACK-V3` |
| `P7 live-probe` | Unchanged profile opened v4; only observational checks ran | `P8`, `ROLLBACK-V3` |
| `P8 offline-audit` | Probe stopped safely; exact post-run SQLite invariants hold | `P9`, `ROLLBACK-V3` |
| `P9 committed` | Final restart is active and live observations pass | Human rollback only |
| `ROLLBACK-V3` | Original v3 and prior image tag restored; service deliberately stopped | Operator review/start |
| `BLOCKED` | No swap occurred, or ownership is uncertain; preserve every artifact | Human recovery only |

The script must persist its current phase outside the tree being swapped and emit
it in every failure report. It must never infer a phase only from a partially moved
directory layout.

## Operator sequence

### 1. Freshness and profile fence

Immediately before work, re-read continuity and the shared mailbox, require the
approved Agent and Core commits, a clean checkout, and no competing PR that changes
migration, persistence, service installation, or wake gating.

Capture and later compare:

- the installed Quadlet bytes and SHA-256;
- the effective `Exec` arguments, which must contain neither `--wake-intents` nor
  an unapproved option;
- absence of `PublishPort` and preservation of the existing security/resource
  settings;
- the current image ID behind `localhost/gaudere-agent:dev`;
- the exact candidate Agent commit, Core pin, built image ID, and build log;
- the active profile name, model name, and secret *name only* (never secret data).

The service must be stopped explicitly and reported `inactive`. Acquire the same
nonblocking `state.db.lock` used by the owner. A failed stop, ambiguous unit state,
or failed flock is a hard stop.

### 2. Quiescent v3 preflight

Under the flock, require all of the following before building a migration staging
tree:

- `PRAGMA user_version = 3` and `PRAGMA integrity_check = 'ok'`;
- the exact expected v3 SQLite objects and no wake-intent table, indexes, or
  triggers;
- no nonterminal Task (`status IN (0,1,2)`);
- no nonterminal Action (`status IN (0,1,2)`);
- no active lease, WAL surprise, or second owner;
- a canonical snapshot of every non-wake schema object and every row, including
  Task result metadata, Actions, and provider budget rows;
- live-control reports captured before stopping for
  `production-openai-first`, `production-reflection-first`, and `budget`.

Rejecting all nonterminal work is intentional. `WorkController::start()` schedules
an immediate dispatch cycle, so merely rejecting provider Tasks would not prove a
side-effect-free service restart.

The durable budget rows must remain byte-for-byte unchanged throughout the gate.
Derived fields such as `in_window_used` or cooldown can legitimately change as time
passes; compare them for consistency with the same durable rows and current clock,
not as frozen strings.

### 3. Runtime-image prerequisite on untouched v3

Before any candidate build command, resolve the image ID currently behind
`localhost/gaudere-agent:dev`, tag that exact ID as
`rollback-before-schema-v4-<stamp>`, and verify that the rollback tag resolves back
to the captured ID. This ordering is mandatory because the historical build path
defaults to `localhost/gaudere-agent:dev`; an unprotected build could otherwise
destroy the only known rollback identity.

Build the candidate from the exact approved clean checkout and Core pin under a
distinct candidate tag, never directly as `localhost/gaudere-agent:dev`. Verify its
Agent/Core provenance, then run provider-free disposable checks showing that the
exact candidate image opens both an unmodified v3 restore and an already migrated
v4 restore without `--wake-intents`.

Only after those checks pass may `localhost/gaudere-agent:dev` be repointed to the
verified candidate ID, without editing the Quadlet. Start the unchanged profile
against the untouched v3 state, then prove:

- the service becomes active and the expected main-worker readiness is logged;
- no explicit-wake-enabled log appears;
- an observational `wake` lookup returns exit code 4 and exactly
  `explicit wake capability is not enabled in this service`;
- the two historical Task reports and durable provider budget remain unchanged;
- no Task, Action, provider permit, or external effect was created.

Stop normally and require the durable `safe` shutdown evidence. Under the flock,
recheck schema v3 and the complete preflight snapshot. Any failure restores the
prior image tag while the database is still untouched and leaves the service
stopped. This is an independent go/no-go boundary before migration.

### 4. Fresh backup and independent restore

With the service inactive, use `scripts/backup-state.sh` so the archive is created
under the same flock contract. Verify its checksum, archive member safety, absence
of an archived lock file, and archive immutability.

Restore it once into the staging tree and once into a separate disposable rollback
verification tree. The latter must open as v3 with the prior image and match the
canonical source snapshot. This proves that recovery does not depend solely on the
directory later renamed to `state.pre-v4-<stamp>`.

### 5. Migrate and prove only the staging copy

Run the candidate image offline, rootless, read-only, capability-dropped, with no
secret and `--network none`, mounting only the staging tree. The sole migration
invocation is `--check --wake-intents`; it must move exactly v3 to v4.

Then require the same assertions already proven by
`scripts/validate-schema-v4-migration-copy.sh`:

- `user_version = 4`, integrity is `ok`, and the precise wake-intent table, two
  indexes, and three triggers exist;
- there are zero wake-intent rows;
- the canonical non-wake schema-and-row snapshot equals the v3 source snapshot;
- all provider budget rows are unchanged;
- a second opt-in check is idempotent;
- a default `--check` without `--wake-intents` reopens v4, emits no explicit-wake
  capability log, creates nothing, and leaves the snapshot unchanged;
- the representative historical Task reports are exact;
- the independently restored rollback copy remains schema v3 and unchanged;
- the archive and checksum are unchanged.

### 6. Pre-swap fence and directory swap

Immediately before the first rename, repeat the inactive-service check, nonblocking
flock, production schema-v3 check, full source snapshot, nonterminal-work query,
candidate image ID, installed-profile checksum, and free-space/same-filesystem
checks. Any drift discards only disposable staging and stops before the swap.

With a cleanup handler armed and the phase durably recorded:

1. rename `state` to `state.pre-v4-<stamp>`;
2. rename `.state.v4-work-<stamp>` to `state`;
3. keep the service stopped;
4. validate installed v4 with the candidate's default `--check`, never with the
   wake flag;
5. recheck the v4 shape, zero wake rows, complete non-wake snapshot, Task reports,
   and durable budget rows.

The swap is by directory rename, never by copying files into the installed tree.
The v3 rollback tree and backup remain immutable after this point.

### 7. Live probe, offline audit, and final restart

Start the existing unit without reinstalling or regenerating its Quadlet. During
the first live probe, use only owner-mediated observational control:

- require `active` and the normal readiness log;
- require absence of the explicit-wake-enabled log;
- inspect both historical Tasks and the budget;
- run only an observational `wake` lookup and require the disabled exit-4 reply;
- do not call `echo`, `openai`, `reflect`, `accept-wake`, or `revoke-wake`.

Stop the probe normally and require `safe`. With the service inactive and the flock
held, perform the exact v4 shape, zero-wake-row, full snapshot, no-nonterminal-work,
and budget audit again. This stop is necessary to obtain exact SQLite evidence
without violating the live owner's boundary.

Finally start the unchanged unit a second time. Require `active`, the same disabled
wake observation, exact historical Task reports, and a budget report consistent
with unchanged durable consumption rows. Recheck that the installed Quadlet hash
and image ID did not drift. Only then record `P9 committed`; leave the service
active and retain the v3 tree, backup, checksum, prior image tag, and logs.

## Why schema v4 does not activate wake

Schema version and capability activation are independent gates:

1. On current main, the default option value is false; only the explicit
   `--wake-intents` argument changes it.
2. Without that flag, `main.cpp` constructs no `WakeIntentStore`,
   `WakeIntentRuntime`, or `ExplicitWake`.
3. `WorkController` receives a null wake runtime, so it reconciles and schedules
   only ordinary Task lease deadlines. It does not poll and has no wake-intent
   deadline source.
4. `LiveControlProcessor` receives a null explicit-wake pointer and returns the
   fixed disabled reply before any wake lookup or transition.
5. The unchanged OpenAI Quadlet contains no wake flag. Its byte hash is fenced
   before and after the gate.
6. Offline and live audits prove the migrated wake table remains empty. No
   successor Task or provider consumption is permitted.

Thus v4 merely makes the dormant durable representation available. It does not
make the installed service wake-capable.

## Automatic rollback contract

Any failure after the first production-tree rename and before `P9 committed`
triggers rollback. The handler must:

1. stop the unit, prove it inactive, remove the canonical Quadlet source, and
   reload systemd so autostart is durably disarmed;
2. acquire the state flock;
3. move an installed v4 tree, if present, to `state.failed-v4-<stamp>`;
4. move the untouched `state.pre-v4-<stamp>` back to `state`;
5. retain the exact rollback profile in the transaction workspace without
   reinstalling or restarting it automatically;
6. validate v3 integrity, complete snapshot, historical Tasks, budget rows, and
   profile checksum offline;
7. leave both service and autostart disarmed and print the exact retained artifacts
   and recovery command for human review.

If inactivity or flock ownership cannot be proved, the handler must perform no
rename and report manual recovery. If a rollback rename fails, it must delete
nothing, preserve every remaining tree, print the observed layout, and leave the
unit stopped. It must never start a service against an ambiguous state tree.

After `P9 committed`, rollback is still possible but is a new explicitly authorized
maintenance operation using the retained v3 tree and prior image; it is not an
automatic cleanup action.

## Failure matrix

| Failure point | Required response | Required end state |
| --- | --- | --- |
| Stale continuity/base, open conflicting PR, profile drift, active service, or flock refusal | Stop before mutation | Original service/state unchanged |
| Candidate build/provenance or disposable reopen fails | Keep prior image tag; stop | Original v3, service stopped |
| Candidate live-on-v3 proof fails | Stop, restore prior image tag, revalidate v3 | Original v3, service stopped |
| Backup/checksum/restore safety fails | Remove only disposable restore when safe | Original v3 and prior backups unchanged |
| Staging migration or any v4 invariant fails | Retain failed staging for diagnosis | Original v3, service stopped |
| Pre-swap recheck detects drift | Do not rename | Original v3, service stopped |
| First rename succeeds, second rename fails | Restore `state.pre-v4-*` immediately | Original v3, service stopped |
| Installed default reopen or offline invariant fails | Execute automatic rollback | Restored v3 plus retained failed v4 |
| First start, live observation, or safe stop fails | Prove inactive, then automatic rollback | Restored v3, service stopped |
| Post-probe offline audit or final start fails | Execute automatic rollback | Restored v3, service stopped |
| Stop/flock cannot be proved during rollback | No filesystem move; emit manual instructions | All trees retained; no claimed recovery |
| Rollback move/validation fails | Delete nothing; emit exact layout and checksums | Service stopped; human recovery possible |

Every post-swap row above also requires the canonical Quadlet source to remain
absent and autostart disarmed. The power-loss and rename-gap decision table plus
the exact operator ordering are documented in
[`production-schema-v4-autostart-fence-recovery.md`](production-schema-v4-autostart-fence-recovery.md).

## WorkerDev implementation and tests

The implementation should be a new production-specific script, not an extension
that weakens the disposable-copy validator. It must default to dry refusal unless
all pinned inputs and paths are explicit. It must support deterministic failure
injection at every phase for CI and must never contain provider credentials.

Required provider-free tests:

1. **Success fixture:** start from a rich v3 fixture containing Actions, both
   historical Task result shapes, metadata, and multiple budget-consumption rows;
   prove the complete sequence ends active on v4, wake rows remain zero, the
   profile hash is unchanged, v3 rollback and backup remain, and every non-wake row
   is equal.
2. **Image compatibility:** prove the selected candidate opens v3 and v4 without
   the flag; reject a candidate that cannot default-reopen v4 or whose image ID
   changes after the fence. Capture and verify the prior `:dev` rollback identity
   before any build, build only under a distinct candidate tag, and reject rollback
   tag drift. The reusable provenance/tag-drift mechanism belongs to issue #50;
   the staged deployment must consume its reviewed contract rather than duplicate
   it.
3. **Wake dormancy:** statically assert both shipped Quadlets omit
   `--wake-intents`; run current main on v4 without the flag; require the disabled
   live-control reply, no wake log, no wake row, no scheduled wake deadline, and no
   new Task.
4. **Quiescence negatives:** reject every Task status 0/1/2 and Action status
   0/1/2, a held lock, an active/activating unit, schema other than exactly v3,
   unexpected wake objects, WAL/state drift, or a changed profile.
5. **Archive/path negatives:** retain the current tests for missing/wrong checksum,
   unsafe archive member, symlinked or ambiguous paths, archived lock, different
   filesystem, collision with rollback/failed paths, and insufficient space.
6. **Failure injection:** inject failure after each rename, installed check, first
   start, live probe, probe stop, offline audit, and final start. Each case must
   either restore the exact v3 snapshot and prior image while stopped or prove that
   no unsafe move occurred. Failed v4 evidence must remain.
7. **Restart behavior:** simulate `Restart=on-failure`; rollback may proceed only
   after an explicit stop and verified inactive state.
8. **Budget/time boundary:** keep durable consumption rows fixed while advancing
   the clock across cooldown and 24-hour-window boundaries; derived live fields may
   change, but no new permit may appear in storage.
9. **Installer recovery prerequisite:** test that the separately updated OpenAI
   installer accepts exactly schemas 3 and 4, rejects all others, and renders bytes
   identical to the current no-wake Quadlet. That isolated recovery change belongs
   to issue #51, not to the staged-deployment implementation.
10. **Static hygiene:** `git diff --check`, shell syntax/lint where available, no
    secret values, no TCP publication, no polling loop, and no provider-capable
    command in the test path.

CI may use fake `systemctl`/`podman` front ends for exhaustive failure injection,
plus a real rootless disposable container run for the default v3/v4 reopen proof.
No CI or local test may mount the production state, production secret, or provider
network.

## Residual risks and stop conditions

- The current mutable production image tag is not sufficient provenance. Record
  exact image IDs and keep both IDs until human cleanup.
- The OpenAI installer compatibility gap is a recovery blocker, even though the
  installed profile itself can remain unchanged. Do not hide it by bypassing the
  installer check.
- The OpenAI profile permits outbound networking. The all-nonterminal-work fence
  and unchanged durable provider-consumption rows are therefore mandatory evidence
  that restart produced no provider effect.
- `Restart=on-failure` can race a failed validation. Explicitly stop and prove the
  unit inactive before any rollback rename.
- SQLite `--check` opens the database read-write. Run it only on disposable or
  stopped, flock-owned trees and compare the complete logical snapshot afterward.
- A clock boundary can change window/cooldown observations without consumption;
  durable budget rows are authoritative.
- A required service-profile change, wake flag, Core redesign, secret operation,
  provider call, nonterminal durable work, ownership ambiguity, or inability to
  retain the complete v3 rollback tree is a hard stop and must be returned to Sol.
