# Local Goose retry operator trust floor ledger v1 — code-only, no authority

This is an **isolated test-only C++17 / SQLite implementation**. It is not
connected to `main.cpp`, `Makefile.am`, `Containerfile`, Fedora, B10,
a Task store, provider or dispatcher. It cannot authenticate an operator,
enroll a trust root, authorize a retry or decide that a human saw text.

## Why a durable floor is needed

PR #283 verifies a *root-signed* operator trust snapshot with an
independently trusted minimum generation. A signed generation-7 ACTIVE
snapshot remains cryptographically valid after an operator revokes that
key in generation 8. A controller must retain a trusted generation floor
across normal restart and compare it before accepting any older snapshot.

This module persistently holds **one root fingerprint and an integer
minimum generation**, not a private key or any permission. It starts
in an isolated SQLite schema v1 with floor 1 and never permits a lower
floor, an in-place reset, automatic retry or another root fingerprint
through its update API.

### State

`(slot=1, root_fingerprint, minimum_generation, last_transition_sha256)`

The evidence hash is opaque caller input. It is **not** a signed operator
approval and **not** proof that the caller verified a root-signed
policy. The initializer hash is only an explicit *test/bootstrap marker*
and is not real enrollment.

The consuming controller MUST independently authenticate the caller,
verify the actual root-signed status/generation and its key/root
identity, and gate the write on genuine operator authorization before
calling `advance_after_external_signature_review()`. This API does none
of those things. A malicious writer possessing file access could raise
the floor to deny service. Nothing in this code should be exposed via
Drive/B10 or user-provided paths.

### Atomic compare-and-swap

An existing writer obtains `BEGIN IMMEDIATE`, rereads the persisted
root and floor, compares **both** with the independently supplied root
fingerprint and expected prior generation, then conditionally updates
to a strictly greater floor with an evidence digest. A single
`sqlite3_changes()==1` and successful COMMIT returns
`advanced_no_authority`. Stale concurrent writers, wrong roots,
non-increasing numbers and malformed digests fail closed. There is no
transaction with the challenge nonce ledger, consent, grant, lease or
Task/ACK history.

If the COMMIT response is unknown, return
`unavailable_or_commit_uncertain`. The caller must inspect the durable
state and seek independent review; it must never replay a grant or
assume this counter is authority.

### File boundary

The test-only store requires an absolute, canonical path with a
pre-existing **owner-0700 immediate parent directory**, a regular
owner-0600 SQLite file with exactly one hardlink and no final symlink,
and no `-wal`, `-shm` or `-journal` companion on opening.
`create_new_isolated` uses `O_EXCL|O_NOFOLLOW` and NEVER opens an
existing path. Existing modes never create missing files or change
the enrolled root. Schema SQL, schema version and journal mode are
checked. The database uses DELETE journal and synchronous FULL.

This does not verify every ancestor directory in the path; a future
operator-controlled controller must select a trusted top-level storage
location and not accept arbitrary caller paths.

**Important residual risk:** file owner and mode plus SQLite durability
do not prevent an adversary with filesystem backup/restore or same-UID
write authority from restoring an *older copy of the entire database*.
Even normal fsync durability cannot prove an external monotonic
counter. Genuine anti-rollback against that adversary requires an
independent trusted generation checkpoint or hardware-backed storage.
Do not advertise this component as proof of irreversibility under host
compromise.

## Synthetic end-to-end Fedora44 verification

The dedicated workflow compiles strict C++17 `-Werror` against the
#283 root-signed policy and #282 local key pin. Its tests:

1. Create a private SQLite floor 1, verify an exact synthetic
   Ed25519-signed ACTIVE generation 1, apply a *separately reviewed*
   generation 8 floor transition and confirm the old valid signed
   ACTIVE generation is rejected.
2. Close and reopen read-only/re-writable; the higher floor survives
   normal restart, read-only cannot advance, no automatic reset.
3. Reject wrong root, stale expected generation, old/same generation,
   malformed digest and out-of-range SQLite int64 generation.
4. Launch two threads holding separate writer connections against the
   same prior generation. At most one advances, with durable floor 2.
5. Reject absent files, duplicate creation, permissive file/parent
   modes, symlink, hardlink and suspicious SQLite companion files.

No real root or private keys are generated or stored; all signatures
use deterministic test-only in-memory keypairs. No Fedora host
commands are executed by this repository PR.

## Next real-world gates

- Trusted operator key/root enrollment and ownership proof, with
  secure root fingerprint and independent monotonic checkpoint.
- Root-signed policy verification wired **before** any counter update,
  and a non-bypassable service identity boundary; not only an opaque
  evidence digest supplied by a caller.
- Trusted clock/CSPRNG challenge issuer and human signing UX showing
  the exact message; reconnect trust state after crash.
- Fresh historical retry eligibility, cross-store transactional
  grant/lease/intent/Task+ACK with explicit manual-review uncertainty.
- Separate security review and **express operator authorization** for
  any future real Fedora installation or activation.

A successful floor advance is **neither human consent nor retry
authorization**. The live provider remains OFF for this feature.
