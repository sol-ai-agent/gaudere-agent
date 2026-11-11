# Retry operator root pin v1 — offline read-only trust preflight

**Source-only, not installed and not operational.** This is the next
isolated safety stage after #283, #284 and #285. Nothing in this tranche
generates, enrolls, signs, rotates or revokes a real operator key or
activates a retry.

## Threat reduced

The #285 revocation bridge correctly required a root-signed REVOKED
Ed25519 tombstone, but its public API accepted a root public key and
fingerprint supplied together by the caller. A caller able to select
both values could sign an arbitrary tombstone using its own test key
and supply its matching fingerprint: the signature would prove only
self-consistency, not independently established root identity.

This stage replaces that **public floor mutation path** with a handle
that cannot be fabricated through its public C++ constructor:

`LocalGooseDialogueResponderRetryRootPin`

It may be constructed **only** by a read-only protected-file loader
that also checks the Ed25519 public key against an expected SHA-256
fingerprint supplied from outside the candidate file and outside the
signed request/Drive. The public bridge now accepts this handle, not
raw caller-selected public-key + fingerprint parameters. The existing
#285 signature/REVOKED/status/time/generation checks and SQLite CAS
remain intact.

## Exact local public root file

Canonical bytes are:

```text
gaudere-retry-operator-root-ed25519-pin.v1
operator_id=bertrand
public_key_hex=<64 lowercase hexadecimal characters>
```

The file ends with exactly one LF. No other field, blank line,
whitespace normalization, uppercase hex or all-zero public key is
accepted.

Its root fingerprint uses the existing #283 domain-separated digest:

`SHA256("gaudere-retry-operator-root-ed25519-key-v1|" + 32 raw bytes)`

The file is **public-key only**, yet it must be a regular file owned
by the running UID, mode `0400`, with exactly one hard link.

The source-only loader walks all components of the **absolute,
canonical pathname** using directory descriptors and
`openat(O_DIRECTORY|O_NOFOLLOW)`; symlink components and
`.` / `..` / empty components are refused. Intermediates must
be root-owned or current-UID-owned and non-writable by group/others.
The special case of an exact root-owned `01777` sticky directory
such as `/tmp` is accepted **only as an ancestor** above an owner
private directory, for isolated CI tests. The immediate parent must
be current-UID-owned `0700`. The final file is opened by the
verified directory FD with `O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC`,
checked using `fstat` before and after one bounded read, and
compared with the separately supplied expected fingerprint.

The reader never creates or chmods files and never reads private
keys. A newly created key pair or local-file edit cannot enroll itself
through this function.

## Test cases (Fedora 44, C++17, strict Werror)

The isolated regression suite tests:

- Valid synthetic Ed25519 public root pin, fingerprint and repeated
  read-only verification.
- Substitution by a foreign public key, bad/wrong external expected
  SHA-256, missing/relative/noncanonical path.
- Unsafe owner modes, symlink final file, hardlink, symlink ancestor
  and group/world-writable parent/ancestor.
- Canonical header, final LF, trailing bytes, uppercase hex, zero key.
- Compile-time refusal to construct a root pin from raw bytes.
- Root-signed REVOKED floor advances only with a validated handle;
  the former raw public-key+fingerprint bridge call fails compilation.
- Existing #285 replay prevention after restart, forged tombstone,
  wrong operator key fingerprint and concurrent SQLite writers.

No real operator material or secret enters the tests, repository or
Drive. The source files are absent from `main.cpp`,
`Makefile.am`, `Containerfile` and deployed Fedora services.

## What this DOES NOT prove

**This is not genuine out-of-band enrollment.** The loader accepts
an expected fingerprint and a trusted local path as parameters;
an attacker controlling **those inputs** can place its own
protected file and pass its own expected fingerprint. The handle
only makes accidental or direct C++ bypass of the file inspection
harder; it is not a security boundary against a compromised
caller/process, malicious same-UID actor or writable code.

Before real operator authority, a separately controlled enrollment
ceremony must choose and persist the expected fingerprint and root
path, demonstrate owner intent and provide a revocation/rotation
procedure (including trusted clock, external monotonic rollback
checkpoint, and human-reviewed exact retry text). None is claimed
by this PR. In particular, SQLite file permissions do **not**
prevent restoring an old full database backup.

No runtime retry, grant/lease, Task/ACK, provider, Drive/B10/Podman
command, Fedora installation or live service change is permitted by
this code-only stage. Future deployment/activation requires a
separate explicit operator authorization.
