# Provider activation boundary

The compiled OpenAI provider stack is inactive by default. Building or installing
Gaudere Agent does not enable a provider and does not widen container networking.

## Offline enqueue

An operator may create a durable OpenAI Responses task while the service is stopped:

```sh
gaudere-agent --state /path/to/state.db \
  --enqueue-openai task-id "bounded text input"
```

The task kind is `provider.openai.responses`. It remains pending while the normal
service has no OpenAI handler registered. Its two-attempt budget does **not** authorize
a second OpenAI call: after a process death, a second worker attempt only gives
`ProviderTaskHandler` an opportunity to observe the already-existing recoverable
Action and converge the Task to manual review without replay.

## Explicit service activation

OpenAI is registered only when service/check mode receives an explicit model:

```sh
gaudere-agent --state /path/to/state.db \
  --openai-model MODEL \
  --openai-secret SECRET_NAME \
  --secret-dir /run/secrets
```

`--openai-secret` and `--secret-dir` are optional only because they have defaults:

- secret name: `openai-api-key`;
- secret directory: `/run/secrets`.

They are configuration metadata, not credential values. The API key itself is never
accepted as a command-line option or ordinary environment variable.

The OpenAI endpoint is not configurable through service activation. Production
activation always uses the adapter's fixed HTTPS endpoint, so changing local model or
secret-name configuration cannot redirect the Bearer credential to another host.

## Startup preflight

Before registering the provider handler, `OpenAIActivation`:

1. opens the configured secret directory through `FileSecretSource`;
2. loads the named secret into a move-only `SecretValue`;
3. verifies that the credential is a non-empty, printable, single-line Bearer value;
4. validates the model as a bounded printable identifier;
5. lets the preflight secret copy go out of scope and be best-effort wiped.

A missing secret, a common trailing newline, invalid permissions/path traversal, or an
invalid model prevents provider activation before any Task is executed or external
Action is created.

`--check` may be combined with the OpenAI activation options. This validates the
provider wiring and secret preflight, starts no provider task, and makes no network
request by itself.

## Disposable Podman secret validation

After building `localhost/gaudere-agent:dev`, the host can validate the real Podman
secret-delivery path without using a real credential or enabling networking:

```sh
sh scripts/validate-provider-secret.sh
```

The validator:

1. creates a temporary state directory below `~/.local/share/gaudere/validation/`;
2. creates a synthetic Podman secret from standard input with no trailing newline;
3. runs the hardened container with `Network=none` and mounts that secret as
   `/run/secrets/validation-openai-key`;
4. first mounts it as mode `0444` and proves Gaudere rejects group/other-readable
   credential files;
5. then mounts the same synthetic value as UID/GID 1000 and mode `0400` and proves
   OpenAI activation preflight reaches `safe` successfully;
6. removes the synthetic Podman secret on exit.

The real service may remain running because validation uses a separate SQLite state
directory and a separate container invocation. No OpenAI request is made.

Set `KEEP_GAUDERE_VALIDATION_STATE=1` to keep the temporary state directory for
diagnostics. `GAUDERE_IMAGE` and `PODMAN` have the same override purpose as the other
host validators.

## Deployment status

This activation mechanism is compiled but **not deployed as enabled configuration**.
The current Quadlet still has:

```ini
Network=none
```

and contains no `Secret=` line and no `--openai-model` argument. Therefore the real
service still registers only the local effect-free handlers.

The first real secret mount and outbound-network validation remain separate,
operator-visible steps. During the initial phase Bertrand retains direct control over
creating, replacing, and revoking the provider secret. A future Gaudere-owned identity
may be considered separately, with an external human recovery/revocation path retained.
