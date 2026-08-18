# Gaudere Agent

Gaudere Agent is the application runtime built around the reusable
[Gaudere](https://github.com/sol-ai-agent/gaudere) C++ library.

This repository contains application-specific orchestration, configuration,
provider integrations, and deployment assets. Reusable scheduling and
persistence components remain in the Gaudere library.

## Current slice

The executable is deliberately small. It:

- opens a caller-provided SQLite state file for recoverable actions and bounded work tasks;
- recovers both runtimes before entering normal operation;
- starts an event-driven bounded-work controller with an immediate startup wake;
- schedules future recovery at the exact earliest active lease deadline rather than polling;
- waits for `SIGINT` or `SIGTERM` on a dedicated signal-wait thread while all work-runtime transitions remain serialized on the main worker thread;
- stops future work wakes before entering draining and exits only after both runtimes reach the safe state;
- exposes `--check` for a non-blocking startup/recovery/shutdown check;
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

Offline operator test mode:

```sh
gaudere-agent --state /path/to/state.db --echo test-001 "hello Gaudere"
```

The echo mode must not be run concurrently with the service: the current operating
model intentionally has one process owning a given SQLite state database. Reusing
the same echo ID is idempotent and returns the already-persisted successful result.
The parent directory for the state file must already exist.

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
