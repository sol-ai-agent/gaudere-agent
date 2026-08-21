# Live service control

The normal rootless Podman service owns its SQLite state database exclusively. A second `gaudere-agent` process must never be used to submit or inspect work while the service is running.

The deployed service therefore exposes a local Unix-domain control socket **inside the container only**:

```text
/tmp/gaudere-control.sock
```

The socket is created mode `0600`, is ephemeral on the container's writable tmpfs-backed `/tmp`, is not part of durable state or backups, and is not published as a host TCP/UDP port.

`/tmp` is intentional here. In the rootless read-only container, Gaudere runs as UID 1000 and cannot create a socket directly in the root-owned `/run` directory. The earlier real-host disposable validator already proved the `/tmp` path works under the same read-only/rootless constraints. If a private writable `/run/gaudere` submount is introduced later, the runtime path can move there explicitly.

The offline Quadlet remains available as a deterministic rollback profile:

```text
Network=none
```

It mounts no provider secret and does not pass `--openai-model`, so live OpenAI and
reflection submission are rejected by policy even though the control protocol knows
their command shapes. The explicit OpenAI profile permits outbound provider traffic
without publishing an inbound port and registers both bounded task kinds.

Use the repository helper from the host:

```sh
sh scripts/control-service.sh echo demo-001 "Bonjour Gaudere"
sh scripts/control-service.sh task demo-001
sh scripts/control-service.sh budget
```

The helper executes `/usr/local/bin/gaudere-control` inside the already-running `gaudere-agent` container. The client process talks only to the Unix socket. It never opens `state.db`.

The `budget` command is observational. The worker reads the durable OpenAI budget through the existing owner process and reports lifetime/window use, remaining slots, cooldown state and whether the provider is enabled. It does not consume a permit and remains available while OpenAI itself is disabled.

`openai` and `reflect` are available only when the selected service profile explicitly
mounts the restricted provider secret, fixes `gpt-5.6-sol`, and permits outbound
networking. Every new ID can consume one durable provider permit. Run either command
only as a separately authorized provider action:

```sh
sh scripts/control-service.sh openai AUTHORIZED_ID "AUTHORIZED_TEXT"
sh scripts/control-service.sh reflect AUTHORIZED_ID "AUTHORIZED_OBJECTIVE"
```

In the offline profile both commands fail before creating a task:

```sh
sh scripts/control-service.sh openai demo-ai "test"
sh scripts/control-service.sh reflect demo-reflect "test"
```

`reflect` creates one `cognition.reflect.v1` task. Its provider output must match the
strict decision schema documented in
[`bounded-reflection-v0.md`](bounded-reflection-v0.md). A `propose_wake` decision is
persisted for inspection but cannot create a successor task or scheduler deadline.

The socket thread itself never mutates the Runtime or SQLite. It validates and queues a bounded command in memory and wakes the existing event-driven scheduler. The main worker thread drains that mailbox and performs durable Task transitions or budget snapshots, preserving the one-owner/single-worker invariant.

Offline maintenance commands (`gaudere-agent --task`, `--cancel`, stopped-state backup, etc.) still require stopping the service first because they open the durable state database directly.
