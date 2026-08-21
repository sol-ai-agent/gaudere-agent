# Gaudere Agent

Gaudere Agent is the application runtime built around the reusable
[Gaudere](https://github.com/sol-ai-agent/gaudere) C++ library.

This repository contains application-specific orchestration, configuration,
provider integrations, and deployment assets. Reusable scheduling and
persistence components remain in the Gaudere library.

## Current slice

The executable is deliberately small. It:

- opens a caller-provided SQLite state file for recoverable actions and bounded work tasks;
- takes an exclusive process-ownership lock beside that state file before opening the runtimes;
- recovers both runtimes before entering normal operation;
- starts an event-driven bounded-work controller with an immediate startup wake;
- schedules future recovery at the exact earliest active lease deadline rather than polling;
- waits for `SIGINT` or `SIGTERM` on a dedicated signal-wait thread while all work-runtime transitions remain serialized on the main worker thread;
- lets a cooperative running handler observe worker stop through an atomic probe, while the worker thread itself persists any resulting cancellation;
- stops future work wakes before entering draining and exits only after both runtimes reach the safe state;
- exposes offline task submission/inspection/cancellation commands;
- registers two effect-free local production task kinds: `local.echo` and `local.wait`.

The application source defines three provider-agnostic work boundaries:

- `TaskExecutor` starts one bounded task, invokes one handler, and records its
  success, explicit failure, acknowledged cancellation, or manual-review result.
  Handler exceptions become manual review because an external effect may already
  have happened. A worker-stop probe may be exposed to the handler, but durable
  cancellation transitions are still serialized through `TaskExecutor` on the
  worker thread.
- `TaskDispatcher` is a single-owner, one-worker selector. Handlers are registered
  explicitly by task kind; `dispatch_one()` selects only pending kinds that have a
  registered handler and delegates exactly one task to `TaskExecutor`. Unknown
  future provider kinds remain pending and do not block supported work.
- `WorkController` composes the wake Scheduler, bounded-work Runtime, and dispatcher.
  It owns no thread and performs no periodic polling. New in-process work can request
  an immediate wake, while interrupted active work schedules its exact durable lease
  recovery deadline.

`local.echo` returns bounded text input as a durable result. `local.wait` accepts an
integer duration from 1 to 5000 milliseconds, waits locally in short increments, and
checks the cancellation probe between increments. It performs no external action and
exists to exercise graceful interruption, hard-crash lease recovery, and bounded
attempt semantics before any provider or network capability is introduced.

The next cognition boundary is specified in
[`docs/bounded-reflection-v0.md`](docs/bounded-reflection-v0.md). It is deliberately
limited to one explicitly submitted provider reflection whose JSON decision is
strictly validated and persisted. A proposed wake is observable data only in v0; it
cannot create work or schedule itself.

Normal service mode:

```sh
gaudere-agent --state /path/to/state.db
```

Offline operator commands:

```sh
gaudere-agent --state /path/to/state.db --echo test-001 "hello Gaudere"
gaudere-agent --state /path/to/state.db --enqueue-wait wait-001 1000
gaudere-agent --state /path/to/state.db --task wait-001
gaudere-agent --state /path/to/state.db --cancel wait-001 "operator request"
```

`--enqueue-wait` creates a durable pending `local.wait` task without executing it;
the normal service picks it up on its immediate startup wake. Wait tasks have two
attempts so a hard-killed first execution can be recovered after its lease expires.
`--task` reports durable metadata, attempts, lease/cancellation information, and a
terminal result when one exists. Text fields are escaped before being written to the
terminal. `--cancel` cancels pending work immediately; active work receives a durable
cancellation request that is finalized cooperatively by its worker or by lease
recovery.

On graceful process shutdown, the signal thread only publishes stop and wakes the
Scheduler. It does not mutate the work Runtime or SQLite. A cooperative running
handler may observe that stop request and return `cancelled`; the main worker then
persists `cancel_requested` followed by `cancelled` before entering draining. If the
process dies between those durable transitions, normal lease recovery can finish the
cancellation later.

All modes use an exclusive sibling lock file (`state.db.lock`). A second
`gaudere-agent` process targeting the same state database fails immediately while the
first owns it. This enforces the current one-process-owner model instead of relying on
operator discipline alone. In normal deployment, stop the service before running an
offline command:

```sh
systemctl --user stop gaudere-agent.service
```

Reusing the same echo or wait ID is idempotent and never overwrites the original
durable task. The parent directory for the state file must already exist.

## Persistent state and migration

The rootless Fedora/Podman deployment stores Gaudere's persistent state on the host
outside the source checkout and outside the container image:

```text
~/.local/share/gaudere/state/
```

The Quadlet mounts that directory as `/var/lib/gaudere` inside the container, and
the service currently uses:

```text
~/.local/share/gaudere/state/state.db
```

as its SQLite state database. This database is durable application state, not a
build artifact or cache. It contains recoverable actions, bounded tasks, leases,
and durable task results, and must be included in backup and machine-migration
procedures. Re-cloning the Git repositories or rebuilding the container image does
not restore this state.

`state.db.lock` is only a local process-ownership coordination file. It contains no
semantic Gaudere state and may be recreated on a new machine. The SQLite database
(and any SQLite side files present while it is live) are the important data.

The current real Fedora checkout used for development is:

```text
~/Documents/Codes/Projets/ia/gaudere/gaudere-agent
```

That path is a checkout location, not persistent runtime state; it may change on a
new system without changing where the database is restored.

### Stopped-state backup

The host backup script archives the complete durable state directory except for the
coordination-only `state.db.lock`. It acquires the same advisory lock used by
`gaudere-agent` and refuses to proceed if any process currently owns the database.
It therefore remains safe even if Gaudere is later launched by something other than
systemd.

For the normal Fedora deployment:

```sh
systemctl --user stop gaudere-agent.service
archive=$(sh scripts/backup-state.sh)
printf '%s\n' "$archive"
systemctl --user start gaudere-agent.service
```

The default backup directory is:

```text
~/.local/share/gaudere/backups/
```

Each backup is a `gaudere-state-*.tar.gz` archive accompanied by a `.sha256` file.
The script holds the state lock until archive creation completes. `GAUDERE_STATE_DIR`
and `GAUDERE_BACKUP_DIR` may override the source and destination directories.

Before restoring or transferring a backup, verify its checksum from its backup
directory:

```sh
sha256sum -c gaudere-state-*.tar.gz.sha256
```

A restore should be extracted into a **new empty directory first**, never directly
over a live state directory. Validate that copy before replacing the current state.
The disposable validator below automates exactly that sequence. Keeping the old
state directory intact until the restored copy has passed validation provides the
rollback path for a real migration.

### Disposable backup/restore validation

After building `localhost/gaudere-agent:dev`:

```sh
sh scripts/validate-state-backup.sh
```

This uses only disposable directories under `~/.local/share/gaudere/validation/`.
It creates durable task state, proves that a live owner blocks backup, creates and
checksums a stopped-state archive, extracts it into a fresh directory, reads the
original durable task from the restored database, and writes a new task afterward to
prove the restored database remains usable. It does not touch the real
`~/.local/share/gaudere/state/` directory.

Set `KEEP_GAUDERE_VALIDATION_STATE=1` to keep the disposable directories for
inspection after the test. `GAUDERE_IMAGE` and `PODMAN` have the same override
semantics as the runtime validator below.

## Real-host runtime validation

After building `localhost/gaudere-agent:dev`, the host-only validation script can
exercise the current runtime invariants without writing to the real
`~/.local/share/gaudere/state/state.db`:

```sh
sh scripts/validate-host-runtime.sh
```

The script creates a disposable state directory below
`~/.local/share/gaudere/validation/`, mounts it with a private SELinux label, and
removes it when validation finishes. The normal systemd service may remain running
because the validation database is separate.

It checks:

- startup/check and durable echo;
- pending offline cancellation;
- exclusive process ownership with a second `gaudere-agent` launched by `podman exec` inside the live validation container, avoiding a second `:Z` relabel;
- graceful SIGTERM cancellation of a running `local.wait`;
- hard SIGKILL leaving a durable first-attempt lease;
- replacement startup, exact lease-expiry recovery without polling, and successful second-attempt completion.

Set `KEEP_GAUDERE_VALIDATION_STATE=1` to preserve the disposable validation directory
afterward for diagnosis. `GAUDERE_IMAGE` may override the image tag and `PODMAN` may
override the Podman command.

## Build

Gaudere core and SQLite persistence must be installed and discoverable through
`pkg-config`.

```sh
autoreconf --install
mkdir build
cd build
../configure
make
make check
```

## Principles

- C++17 and Linux-first development.
- Outbound-only networking on the initial workstation.
- Recoverable, idempotent actions and bounded durable tasks.
- Rootless and least-privilege deployment where practical.
- Provider-specific code kept outside the generic Gaudere core.
- Secrets never committed.

Licensed under the MIT License.
