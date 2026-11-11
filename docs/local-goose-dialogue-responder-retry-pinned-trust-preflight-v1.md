# Protected root + root-signed policy + revocation floor preflight v1

**Source-only and inactive.** This stage adds a pure read-only C++ inspector
which composes the root key file loader from #286, root Ed25519 signature
verification from #283, and an existing SQLite generation floor from #284/#285.

It does **not** implement real human enrollment, key custody, a trusted clock,
external monotonic anti-rollback, root rotation or a responder retry. It is
**not** wired into main, Makefile.am, Containerfile, B10, Fedora production,
Task/ACK or any live service.

## Preconditions and exact evidence

The caller supplies **pre-existing** objects and values:

1. A `LocalGooseDialogueResponderRetryRootPin` whose file loader checked
   strict canonical public key bytes, directory ownership and modes,
   link count and a *separately supplied expected root fingerprint*.
   No public API for constructing a root pin from arbitrary key bytes.
2. A `LocalGooseDialogueResponderRetryTrustFloorLedger` opened explicitly as
   `existing_read_only`. Writer/create-mode ledgers are rejected by this
   preflight. Missing/invalid SQLite state is not silently initialized.
3. An exact canonical root-signed trust snapshot; its signature and trusted
   time window must pass the existing #283 verifier.
4. A separately expected lowercase SHA-256 fingerprint of the operator
   signing key. The candidate signed snapshot cannot choose that expected
   identity.
5. A trusted `now_ms` supplied externally (not proved by this API).

The inspector looks up the floor for the root fingerprint, rejects a snapshot
below `minimum_generation`, rejects a foreign operator fingerprint, and
reports exactly:

- `consistent_active_no_authority`
- `consistent_revoked_no_authority`
- explicit reject codes for mutable floor, absent/wrong floor root, invalid
  expected operator key, signature/validity failure, stale generation, or
  mismatch with the expected operator identity.

The result is **repeatable, observational and inert**. It cannot advance the
floor, enroll or revoke a key, consume a nonce, prepare a grant, submit a Task,
dispatch an intervention or ACK sequence 4. The private mutation bridge for
the floor remains the root-signed REVOKED-only #285/#286 path.

## Threats not solved

- A caller who supplies both its own pin pathname and its matching expected
  fingerprint can still authenticate **itself**. The function receives a
  previously validated handle, but it has no proof of where the expected
  fingerprint/path came from. The trusted operator-controlled enrollment
  controller remains a separate, human-gated design.
- Local SQLite monotonic generations do not defeat a whole-filesystem backup
  restore or same-UID compromise. A separately secured monotonic checkpoint
  remains necessary for a production anti-rollback claim.
- A caller-supplied wall clock can be forged. No clock attestation exists.
- A valid ACTIVE generation newer than a REVOKED floor is merely
  cryptographically consistent: this stage does not authorize reactivation,
  consent or any runtime effect. Any reactivation/rotation needs a new
  authenticated operator ceremony and separate activation gate.
- Signature verification, SQLite floor inspection and any future nonce/Task
  accounting are not performed in a cross-store transaction. This source
  method cannot be used as final live admission authority.
- Read-only inspection is not a snapshot of an actively changing filesystem;
  future Fedora proof must use a consistent quiescent copy or an audited
  snapshot protocol.

## Isolated synthetic proof (Fedora 44)

The test uses synthetic Ed25519 material only and a private temporary
directory. It checks the protected local pin and wrong fingerprint rejection,
root-signed ACTIVE/REVOKED inspection, bogus expected operator identity,
modified signature, expired validity and foreign root mismatch. It proves that
a writable/create floor is rejected, that repeated reads leave the floor
untouched, and that a separate verified REVOKED transition from generation 1
to 8 prevents reaccepting the old signed ACTIVE snapshot after reopening.
It also verifies a newer ACTIVE snapshot has only **no-authority** status.
Strict C++17 `-Werror` compilation and a static no-production-wiring check
are required.

## Next independent gates

1. Establish a genuinely authenticated local operator enrollment controller
   whose root path and fingerprint **cannot** be chosen through a request,
   candidate file, Drive or API caller.
2. Design and test external monotonic rollback evidence and trusted clock.
3. Introduce human-visible exact-message review and signed approval; do not
   confuse cryptographic consistency with proof of presence or consent.
4. CI, security review, merge; then explicit independent Fedora/integration
   and production-effect gates. **No gate is authorized by this PR.**

Provider/OpenAI remains OFF, historical cursor 3 and completion sequence 4
remain untouched, no first dispatch or ACK is performed.
