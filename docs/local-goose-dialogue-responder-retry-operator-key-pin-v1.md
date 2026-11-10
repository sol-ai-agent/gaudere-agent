# Retry generation 1 — local operator public-key pin inspection

Status: **isolated C++17 source and synthetic tests only**. This is not an
enrollment ceremony, human consent, an activated retry or a production change.

## Threat reduced

PR #279 inspected an Ed25519 signature over **exact intervention text**,
but the caller could supply any `separately_pinned_public_key`. PR #281
would then burn an isolated nonce using a matching caller-supplied signature.
Those checks are mathematically valid but do **not** authenticate the human.

This tranche introduces an optional, read-only key-pin boundary.
It accepts only an **existing absolute-path local file** plus an
**independent trust-anchor fingerprint** supplied by a future trusted operator
configuration. It never accepts an identity or fingerprint from Drive, a
candidate signed message or the pin file as proof of authority.

The local file has precisely the following format (one lowercase 64-hex
Ed25519 public key, no extra fields):

```text
gaudere-retry-operator-ed25519-key-pin.v1
operator_id=bertrand
public_key_hex=<64 lowercase hex characters>
```

The pin must be a regular file owned by the running UID, with mode **0400**,
one hard link, no final-path symlink, no missing/trailing bytes and no
unexpected format. The reader uses a single `open(O_NOFOLLOW|O_CLOEXEC)`
file descriptor, `fstat` before and after bounded reads, and never creates
or modifies a file. Only an absolute path supplied by the trusted operator
configuration is allowed. Directories containing the file must themselves
be protected; this pure function does not perform a whole-tree secure
filesystem walk.

The expected fingerprint is computed by the *trusted operator outside
this function*, using the existing #279 convention:

`SHA256("gaudere-retry-operator-ed25519-key-v1|" + 32 raw public key bytes)`

This function **refuses** an absent, malformed or differing expected
fingerprint. A forged file with a fresh attacker's key therefore cannot
replace the independent trust anchor. That says nothing about whether a
legitimate operator actually performed a trusted enrollment.

Once the file and independent anchor match, the function delegates to the
existing #279 exact-message Ed25519 inspector, including request scope,
challenge nonce, expiry and byte-for-byte reviewed intervention text.
Successful result stays
`cryptographically_consistent_no_authority` — a repeatable inspection,
not approval.

## CI

The dedicated Fedora44 synthetic tests compile with `-std=c++17 -Werror`
and exercise:
- a valid synthetic Ed25519 signature and matching pinned file;
- an independently trusted fingerprint rejecting an attacker-controlled
  alternate key file;
- corrupt signature, altered message and expired challenge;
- missing/relative path, unsafe file mode, symlink and hardlink;
- wrong header, extra or truncated bytes, uppercase hex and all-zero key;
- repeated inspection, deliberately showing that this is NOT a one-time
  consumption action.

No real operator private key appears in the source or repository. Tests
derive deterministic ephemeral test keys only.

## Still NOT implemented

- Trusted, human-attended key generation/enrollment/rotation/revocation
  and storage of the independent trust anchor.
- Actual operator interface proving display of the exact intervention and
  genuine approval; possession of a private key alone is not proof of that.
- Secure challenge issuance, trusted wall clock, live historical eligibility
  and safe one-time coupling with the challenge/authority/Task stores.
- Wiring to `main.cpp`, `Makefile.am`, `Containerfile` or any service;
  production grant/lease, Task, ACK or provider activity.
- Any B10/Podman/Fedora host command, installation or credentials.

The next safe development stage is to **define a controlled enrollment and
revocation path**, with a clearly authorized human-managed independent trust
anchor and tests. This code deliberately cannot enroll itself. A real Fedora
deployment/activation would require a separate explicit operator gate.
