# Retry generation 1 — isolated exact signed proof consume-once

Status: **code-only / test-only**. This records irreversible usage of one
matching signed-message challenge. It is NOT an approval, grant, capability,
lease, dispatcher or a production-runtime feature.

## Problem

PR #279 verifies a synthetic Ed25519 signature covering exact intervention
bytes; PR #278 stores an isolated immutable SQLite reservation; PR #280 joins
those records in read-only mode. However a read-only observation is repeatable,
and its reservation could be used by another process before a future effect.
A writer needs to compare the *whole* immutable reservation while holding the
SQLite write lock and mark it spent at most once.

This PR adds:

1. `LocalGooseDialogueResponderRetryChallengeLedger::burn_if_exact()`.
   While holding `BEGIN IMMEDIATE`, verify the full canonical generation-1
   reservation (ID, request, challenge, proposal, scope, lineage, nonce
   fingerprint and issue/expiry), its exact current `reserved` state, and
   absence of prior evidence. A single conditional `UPDATE` to `burned`
   with `sqlite3_changes()==1` commits the evidence SHA-256. Changed
   reservations, already-burned and `manual_review` states always conflict.
2. `consume_local_goose_dialogue_responder_retry_signed_proof_once()`.
   Before opening the writer, revalidate the #279 exact signed-message proof,
   its explicitly supplied public key/actor and bounded challenge clock,
   derive the #280 canonical nonce fingerprint and #278 expected reservation,
   and derive a domain-separated SHA-256 evidence digest over full exact
   signed payload (including raw UTF-8 message bytes), review ID, signer,
   signature, key fingerprint, request, nonce and immutable reservation ID.
   Then call `burn_if_exact` once. There is **no retry on uncertain commit**.

A successful call returns **`burned_no_authority`**, never `approved` or
`ready_to_dispatch`. It makes this nonce unusable; it does not allow any
message or Task. A failed/ambiguous commit returns fail-closed
`ledger_unavailable_or_commit_uncertain`; a later caller must inspect the
durable record through a *separate human review process*, not replay.

## What is and is not atomic

**Atomic inside one SQLite file:** the immutable reservation comparison and
`reserved -> burned` transition under the SQLite writer lock. Two competing
writers cannot both succeed on the same reservation. Reopening after a
successful burn continues to reject the same signed bytes and a different
signature with the same request/challenge/nonce.

**Not atomic:** signature verification happens before the SQLite transaction,
so the caller's clock, external key trust/rotation, human review and historical
Gaudere eligibility may change between those steps. The immutable reservation
does not change through this API, but this does **not** prove fresh consent.
No transaction exists across this ledger and a hypothetical future authority
ledger, Task store, feed ACK or provider action. A lost commit response may mean
the nonce was burned; never automatically replay it.

**No real signer yet:** the public key and clock arrive as parameters.
Supplying an arbitrary chosen key plus a corresponding signature is not trusted
enrollment or human consent. A malicious caller with write access could burn
an untrusted test reservation; this code must not run on production until a
separately authenticated operator ceremony and capability boundary exist.

## Isolated CI checks

Strict Fedora44 C++17 `-Werror` with synthetic key and `-pthread` verifies:

- An exact signed message burns one reserved challenge; a repeat after
  reopen returns conflict and the durable evidence digest remains present.
- Wrong signature, changed final text, expired time, invalid request and a
  different valid request leave the DB byte-for-byte unchanged.
- A well-formed but foreign nonce fingerprint cannot be consumed; direct
  `burn_if_exact` rejects altered canonical fields and malformed digest.
- `manual_review` is absorbing; unknown/nonexistent file, unsafe 0644
  permissions and symlinks fail closed without automatic repair/creation.
- Two concurrently calling threads on separate SQLite connections produce
  **at most one successful burn**, with one conflict or uncertainty. The
  durable final state is `burned`. No automatically repeated effect occurs.
- Static boundary ensures new code has no main/Makefile/Containerfile,
  provider, systemd, Podman, B10, Task, ACK or network wiring.

The older low-level `burn(challenge_id,request_id,evidence_sha256)` method
remains available for compatibility with its earlier isolated tests; it is
*not* a verified signed-proof operation and must not serve as an authority
boundary. A future trusted service must use a reviewed, restricted capability
and enforce all independent gates, not merely call either burn method.

## Still required before any real retry

- Authenticated, operator-owned Ed25519 key enrollment and revocation,
  signing UI that proves display/confirmation of the **exact final text**.
- Trusted CSPRNG nonce issuer and authoritative clock, independent of callers.
- Durable fresh-source revalidation of the historical held human completion
  sequence 4, cursor 3, expected head revision 3, prior terminal intent/
  disposition, no submitted Task, bounded system-turn budget.
- One generation-aware cross-store grant/lease/decision/intent/disposition
  and Task/ACK accounting protocol with crash/restart manual review; no
  modification of historical schema-v3 `(consumer_id,event_id)` uniqueness.
- Distinct CI and isolated Fedora copy proof, manual security review and
  **explicit operator approval** for any production deploy/activation.

No production Fedora file, service, image, provider or database is touched
by this source-only PR. Last user-reported Fedora proof (#274) remains
`evidence_consistent_no_authority`, provider OFF, `Network=none`,
human completion sequence 4 held, consumer cursor 3, no retry Task/ACK.
