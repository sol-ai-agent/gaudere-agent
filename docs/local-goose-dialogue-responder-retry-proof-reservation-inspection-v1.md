# Retry generation 1 — read-only exact proof ↔ reserved challenge inspection

Status: **inert source-only check**, no runtime wiring, no production sidecar,
no human approval and no permission to retry.

## Purpose and existing primitives

PR #279 validates an exact proposed intervention's raw UTF-8 bytes and
Ed25519 signature against a separately provided public key.
PR #278 stores one versioned SQLite challenge reservation with unique
request, challenge, nonce fingerprint and (scope,generation) indices. Before
this tranche, neither component checked that the *same* immutable challenge,
nonce and scope appeared in both artifacts.

`LocalGooseDialogueResponderRetryProofReservationInspector` is a
read-only join across those two pieces of evidence. The return value
`consistent_reserved_no_authority` means ONLY that an exact-message
signature is cryptographically consistent with a **caller-provided**
public key and the matching reservation is currently pending. It does
**not** mean that a real human approved, that the key was properly
enrolled, that the nonce was unpredictable, or that any action is permitted.

## Exact inspection

1. Re-run the entire #279 signature predicate against the provided immutable
   proposal, scope, operator challenge, review text, signature, separately
   pinned public key/actor and clock. Malformed/expired/foreign signatures
   and mismatched messages fail closed *before* any ledger lookup.
2. Derive a versioned nonce fingerprint from the challenge's **exact
   64 lowercase hexadecimal nonce characters**:
   `SHA256("gaudere.cognition.local-goose-dialogue-responder-retry-nonce-fingerprint.v1|" + nonce_hex)`.
   Do not accept uppercase, all-zero, malformed or oversized nonces.
   This binds representations, but it proves **neither randomness nor
   independently trusted challenge issuance**.
3. Canonically reconstruct the #278 generation-1 reservation from one
   explicit expected request ID, exact challenge ID, proposal ID,
   scope/lineage IDs, nonce fingerprint and issue/expiry timestamps.
4. Open the **existing** owner-0600 SQLite file with only #278's
   `existing_read_only` mode. The underlying ledger enforces symlink
   rejection, schema version 1, DELETE journal, no `-wal/-shm/-journal`
   companions, SQLite NOFOLLOW, and no creation/migration. No caller
   may select writer mode through this inspector.
5. Compare **every field** and the canonical `reservation_id` against
   the stored record. Wrong request, nonce, challenge, scope, lineage,
   generation or timestamps fail closed, even if individually well-formed.
6. Accept only an exact `reserved` row with no evidence SHA. Rows in
   `burned` or `manual_review` are unusable. Missing, absent/unsafe
   ledger or SQL failures fail closed.

The result is a repeatable observation and can become stale immediately
after the read. Running this check twice does **not consume** the
reservation. No transaction or cross-store atomicity with a future
grant/lease/accounting file is claimed. In particular,
`consistent_reserved_no_authority` must never be interpreted as a
dispatchable, activated, approved or single-use capability.

## Synthetic CI evidence

Isolated Fedora44 strict C++17 `-Werror` tests generate a **test-only**
Ed25519 key and synthetic proposal/scope/challenge/exact message. They
check: exact signed proof and canonical pending reservation; repeatable
read without byte changes or companions; wrong/altered signature and
message, expired challenge, wrong expected request; mismatched nonce
fingerprint, scope, timestamps; permanently burned/manual-review states
surviving reopen; missing ledger, wrong 0644 permissions, symlinks and
unexpected WAL companion rejected without repairing or creating files.

These tests provide no human presence or real Fedora proof. A fabricated
key and matching signature are valid synthetic test data, **never an
enrolled operator credential**.

## Independent gates still required

- A trusted operator-key enrollment/revocation and signing UX proving that
  the real human saw and confirmed the **exact intervention text**.
- Cryptographically secure nonce generation with authenticated issuance,
  and durable one-time **consumption** tied atomically to the approved
  exact-message proof. #278's `burn` remains an independent, untrusted,
  opaque record until the trusted actor/cross-store crash boundaries are
  designed and tested.
- Fresh inspection of the real historic source stores, held unACKed human
  completion sequence 4, consumer cursor 3, thread head revision 3 and
  absence of a submitted Task. Frozen-copy `PASS` cannot substitute.
- New generation-1-specific grant/lease/decision/intent/disposition,
  one-turn budget and bounded typed dispatcher, without modifying
  historical schema-3 `(consumer_id,event_id)` uniqueness. No automatic
  retry on crash, timeout or manual-review.
- Distinct review, isolated-copy verification and explicit human Fedora
  gate before **any** runtime integration, deployment or production effect.

This tranche changes no `main.cpp`, `Makefile.am` or `Containerfile`.
There is no Task, ACK, provider call, B10, Podman, network or Fedora
execution. Production stays at the last user-reported #274 evidence:
provider OFF, `Network=none`, 10 providers, human sequence 4 still held,
consumer cursor 3, no retry authority or submitted Task.
