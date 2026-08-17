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
- waits for `SIGINT` or `SIGTERM`;
- enters draining and exits only after both runtimes reach the safe state;
- exposes `--check` for a non-blocking startup/recovery/shutdown check.

The application source defines two provider-agnostic work boundaries:

- `TaskExecutor` starts one bounded task, invokes one handler, and records its
  success, explicit failure, acknowledged cancellation, or manual-review result.
  Handler exceptions become manual review because an external effect may already
  have happened.
- `TaskDispatcher` is a single-owner, one-worker selector. Handlers are registered
  explicitly by task kind; `dispatch_one()` selects only pending kinds that have a
  registered handler and delegates exactly one task to `TaskExecutor`. Unknown
  future provider kinds remain pending and do not block supported work.

Both boundaries are currently exercised only by deterministic local tests. There
is deliberately no dispatch loop, polling thread, provider, external action, or
network port yet. Pending bounded tasks may exist durably in the state database,
but the production process does not automatically execute them.

```sh
gaudere-agent --state /path/to/state.db
```

The parent directory must already exist.

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
