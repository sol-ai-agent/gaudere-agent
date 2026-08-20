# Live service control

The normal rootless Podman service owns its SQLite state database exclusively. A second `gaudere-agent` process must never be used to submit or inspect work while the service is running.

The deployed service therefore exposes a local Unix-domain control socket **inside the container only**:

```text
/tmp/gaudere-control.sock
```

The socket is created mode `0600`, is ephemeral on the container's writable tmpfs-backed `/tmp`, is not part of durable state or backups, and is not published as a host TCP/UDP port.

`/tmp` is intentional here. In the rootless read-only container, Gaudere runs as UID 1000 and cannot create a socket directly in the root-owned `/run` directory. The earlier real-host disposable validator already proved the `/tmp` path works under the same read-only/rootless constraints. If a private writable `/run/gaudere` submount is introduced later, the runtime path can move there explicitly.

The Quadlet remains offline at this stage:

```text
Network=none
```

It mounts no provider secret and does not pass `--openai-model`, so live OpenAI submission is rejected by policy even though the control protocol knows the command shape.

Use the repository helper from the host:

```sh
sh scripts/control-service.sh echo demo-001 "Bonjour Gaudere"
sh scripts/control-service.sh task demo-001
sh scripts/control-service.sh budget
```

The helper executes `/usr/local/bin/gaudere-control` inside the already-running `gaudere-agent` container. The client process talks only to the Unix socket. It never opens `state.db`.

The `budget` command is observational. The worker reads the durable OpenAI budget through the existing owner process and reports lifetime/window use, remaining slots, cooldown state and whether the provider is enabled. It does not consume a permit and remains available while OpenAI itself is disabled.

`openai` is intentionally unavailable in the ordinary service until a later deployment change explicitly mounts the restricted provider secret, selects `gpt-5.6-sol`, and widens outbound networking. Until then this command must fail:

```sh
sh scripts/control-service.sh openai demo-ai "test"
```

The socket thread itself never mutates the Runtime or SQLite. It validates and queues a bounded command in memory and wakes the existing event-driven scheduler. The main worker thread drains that mailbox and performs durable Task transitions or budget snapshots, preserving the one-owner/single-worker invariant.

Offline maintenance commands (`gaudere-agent --task`, `--cancel`, stopped-state backup, etc.) still require stopping the service first because they open the durable state database directly.
