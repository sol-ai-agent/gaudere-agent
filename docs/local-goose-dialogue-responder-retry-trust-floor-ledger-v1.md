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

The stored hash is the digest of the exact signature-verified policy
(including the signature and last LF), computed by the #285 bridge, not
an arbitrary digest supplied through its public API. The raw
`advance_after_external_signature_review()` method is **private** and
can only be reached by its friend revocation bridge; a direct unsigned
C++ call is rejected at compile time.

The bridge verifies the independently pinned root public key and SHA256,
the signed operator-key fingerprint, `status=revoked`, generation,
validity window and full Ed25519 signature before updating the floor.
Only a genuine root-signed tombstone can advance it via the public API.
There is **still no actual human enrollment or consent**: a caller that
can substitute the supposedly independent root/key/clock can invent its
own trust policy. A malicious same-UID writer with filesystem access
could also change the database or restore a backup. Nothing in this
code should be exposed to Drive/B10 or arbitrary caller-specified paths.

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
   Ed25519-signed ACTIVE generation 1, commit a valid root-signed
   REVOKED generation 8 using the only public update path and confirm
   the old valid signed ACTIVE generation is rejected.
2. Close and reopen read-only/re-writable; the higher floor survives
   normal restart, read-only cannot advance, no automatic reset.
3. Reject wrong root/key fingerprint, forged/corrupted signature,
   root-signed ACTIVE instead of REVOKED, stale and expired snapshots,
   out-of-range generations and noncanonical signed content.
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
- Cryptographic policy verification is now wired before public counter
  updates, but an **authenticated external service identity** is still
  needed so a caller cannot choose its own trust anchors.
- Trusted clock/CSPRNG challenge issuer and human signing UX showing
  the exact message; reconnect trust state after crash.
- Fresh historical retry eligibility, cross-store transactional
  grant/lease/intent/Task+ACK with explicit manual-review uncertainty.
- Separate security review and **express operator authorization** for
  any future real Fedora installation or activation.

A successful floor advance is **neither human consent nor retry
authorization**. The live provider remains OFF for this feature.
