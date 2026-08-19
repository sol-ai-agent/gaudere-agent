# Secret source boundary

Gaudere does not currently receive or use any real provider secret. This document
records the pre-activation boundary and the intended least-privilege delivery path.

## Application contract

`SecretSource` resolves a named secret into a move-only `SecretValue`.

`SecretValue` deliberately has no implicit string or stream conversion. Its owned
bytes are overwritten on destruction as a best-effort reduction of credential
lifetime in process memory. This is not a claim that all compiler, allocator, kernel,
or library copies can be eliminated; provider adapters must still avoid logging or
persisting secret material.

`FileSecretSource` is rooted at one directory, `/run/secrets` by default. It:

- accepts only a single conservative filename component;
- rejects empty names, `.`/`..`, slashes, whitespace, and other unexpected characters;
- opens the root directory and secret files with `O_NOFOLLOW`;
- uses `openat()` relative to the already-open root directory;
- accepts regular files only;
- rejects files with any group/other permissions;
- enforces a configurable maximum size (16 KiB by default);
- preserves the secret bytes exactly, including a trailing newline if one exists;
- treats a missing secret differently from an invalid or insecure secret.

The production `main` does not construct `FileSecretSource`, and the current Quadlet
does not mount any secret.

## Planned Podman delivery

Podman/Quadlet supports runtime secrets as mounted files. When a concrete provider is
selected, the intended shape is a pre-created Podman secret exposed read-only beneath
`/run/secrets`, owned by the container's Gaudere UID/GID and with mode `0400`.

An illustrative future Quadlet entry is:

```ini
[Container]
Secret=gaudere-provider-api-key,target=provider-api-key,uid=1000,gid=1000,mode=0400
```

This line is intentionally **not** present in the deployed Quadlet yet. Secret
creation, naming, rotation, provider activation, and network widening remain separate
operator-visible steps.

The secret should be created from standard input (or another protected local source)
rather than committed to a repository or baked into an image. The application will
read the mounted file only when the corresponding provider is explicitly enabled.

## Why file mount instead of environment

The initial design chooses Podman's file-mounted secret mode rather than exposing the
credential as a container environment variable. This keeps the application contract
path-based, makes permissions explicit, and avoids making the environment the normal
credential transport.

## Tests

Offline tests use temporary directories and synthetic non-secret values. They prove:

- exact byte loading and move-only value semantics;
- missing-secret behavior;
- path traversal/name rejection;
- symlink rejection;
- broad-permission rejection;
- size-limit enforcement;
- rejection of a symlink used as the secret root directory.

No test requires a real credential or network access.
