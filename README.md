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
- stops future work wakes before entering draining and exits only after both runtimes reach the safe state;
- exposes `--check` for a non-blocking startup/recovery/shutdown check;
- exposes offline task inspection and cancellation commands;
- registers one production task kind, `local.echo`, whose only effect is to return its bounded text input as a durable task result.

The application source defines three provider-agnostic work boundaries:

- `TaskExecutor` starts one bounded task, invokes one handler, and records its
  success, explicit failure, acknowledged cancellation, or manual-review result.
  Handler exceptions become manual review because an external effect may already
  have happened.
- `TaskDispatcher` is a single-owner, one-worker selector. Handlers are registered
  explicitly by task kind; `dispatch_one()` selects only pending kinds that have a
  registered handler and delegates exactly one task to `TaskExecutor`. Unknown
  future provider kinds remain pending and do not block supported work.
- `WorkController` composes the wake Scheduler, bounded-work Runtime, and dispatcher.
  It owns no thread and performs no periodic polling. New in-process work can request
  an immediate wake, while interrupted active work schedules its exact durable lease
  recovery deadline.

`local.echo` is the first deliberately harmless production capability. It uses the
same durable submission, dispatch, lease, result, and safe-shutdown path that future
handlers will use, but performs no external action and requires no network, secret,
subprocess, or host capability.

Normal service mode:

```sh
gaudere-agent --state /path/to/state.db
```

Offline operator commands:

```sh
gaudere-agent --state /path/to/state.db --echo test-001 "hello Gaudere"
gaudere-agent --state /path/to/state.db --task test-001
gaudere-agent --state /path/to/state.db --cancel test-001 "operator request"
```

`--task` reports durable metadata, attempts, lease/cancellation information, and a
terminal result when one exists. Text fields are escaped before being written to the
terminal. `--cancel` cancels pending work immediately; active work receives a durable
cancellation request that is finalized cooperatively by its worker or by lease
recovery.

All modes use an exclusive sibling lock file (`state.db.lock`). A second
`gaudere-agent` process targeting the same state database fails immediately while the
first owns it. This enforces the current one-process-owner model instead of relying on
operator discipline alone. In normal deployment, stop the service before running an
offline command:

```sh
systemctl --user stop gaudere-agent.service
```

Reusing the same echo ID is idempotent and returns the already-persisted successful
result. The parent directory for the state file must already exist.

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

Before copying or backing up the live database directly, stop the user service so
that the current single-owner model has no open writer:

```sh
systemctl --user stop gaudere-agent.service
```

For a machine migration, preserve the whole `~/.local/share/gaudere/state/`
directory after the service has stopped, restore it for the target user, then
reinstall/rebuild the service. The repository checkout and the persistent runtime
state are deliberately separate.

The current real Fedora checkout used for development is:

```text
~/Documents/Codes/Projets/ia/gaudere/gaudere-agent
```

That path is a checkout location, not persistent runtime state; it may change on a
new system without changing where the database is restored.

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
