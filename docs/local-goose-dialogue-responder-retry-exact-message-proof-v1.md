# Retry generation 1 — exact intervention message signature predicate

Status: **isolated source + synthetic CI only**. No human enrollment, no
operator UI, no approval service, no production code path or Fedora change.

## Problem closed in this tranche

PR #276 prepared an inert retry grant-proposal; #277 verified Ed25519
signatures over an exact *proposal and challenge*; #278 added a separate
one-time challenge reservation/burn ledger. However the v1 signature did
**not bind the final intervention text**. It therefore cannot be used to
authorize text produced after a proposed approval. A new, explicitly
domain-separated signature contract is necessary.

`LocalGooseDialogueResponderRetryExactMessageProof` introduces an immutable
canonical `ExactMessageReview` containing the exact, bounded UTF-8
intervention text and its SHA-256, plus all of these fixed bindings:

- The generation-1 proposal, exact admitted scope/lineage, challenge ID and
  nonce, and the historical event, result SHA-256 and triggering Task ID.
- Completion sequence **4**, expected preferred thread revision **3**,
  one system responder `system-responder-v1`, message kind
  **intervention** and a single system-turn budget.
- A dedicated signing-domain version, challenge issue/expiry timestamps
  and the specific decision `approve_exact_retry_message_v1`.
- The operator ID `bertrand` and a **separately pinned** Ed25519 public
  key. The caller may not provide an arbitrary key as a substitute for
  the missing trusted enrollment ceremony.

The signed bytes include the full exact message, its SHA-256 and byte
length. **No normalization, rewriting, post-signature edit or implied
message generation** is permitted. The review SHA-256 identity is
versioned and canonical. The old proposal-only #277 signature domain
is incompatible and explicitly tested against cross-version replay.

## Text safety boundary

Messages are nonempty and at most **4,096 UTF-8 bytes**, with strict
well-formed UTF-8 and no implicit repair or normalization. Reject overlong
sequences, surrogates, code points above U+10FFFF, NUL, all ASCII/C1 controls
except line feed, and selected invisible/bidirectional controls including
direction overrides, directional isolates and zero-width marks.

This excludes obvious display tricks, but it is **not** a complete
Unicode confusable, font or UI security proof. The real trusted UI must
show the exact bytes, digest, message purpose, origin and limits and must
clearly distinguish composed/decomposed Unicode from a changed message.
Synthetic tests demonstrate that visually similar `Café` and decomposed
`Cafe + combining acute` have different signed identities.

## Cryptographic inspection

The predicate reconstructs the canonical challenge through the existing
#277 factory and validates matching #276 proposal/#275 scope. It enforces
the 5-minute challenge window and 15-minute proposal cap already defined
by those contracts, and verifies Ed25519 against an independently
pinned public key. Modified text, hash, challenge, speaker, sequence,
revision, message kind, actor, decision, key or signature are rejected.
The result is intentionally named:

`cryptographically_consistent_no_authority`

This means ONLY that the bytes verify against the supplied key. It does
**not** prove real user identity, possession of an enrolled operator key,
a completed human review, a live production state, or permission to reply.
The CI test's private key is deterministic synthetic data; it must
never be enrolled or deployed.

## Outstanding mandatory gates

The following are unresolved and **not** silently bridged by this
predicate:

1. Trusted, separately authorized operator-key enrollment/rotation and
   operator-controlled signing UX that displays exactly the message and
   its immutable context.
2. A trusted cryptographically random nonce issuer with durable issuance
   and atomic one-time burn linked to the **exact v2 signed payload**.
   #278's ledger only stores opaque fingerprints, not verified consent.
   It cannot be treated as permission, and a crash between stores must
   fail to durable manual review.
3. Fresh, independent verification of the real production lineage,
   history, unACKed human sequence 4, consumer cursor 3, preferred
   head revision 3, expired historical authority, no Task and provider OFF.
4. Separate generation-aware grant/lease/decision/intent/disposition
   accounting and typed bounded dispatcher support, all with crash/
   restart and concurrency proofs and a distinct explicit Fedora gate.
   Historical schema-v3 `UNIQUE(consumer_id,event_id)` and its
   disposition must remain unchanged. No direct Task submission or ACK.
5. Review of the final UI rendering and text integrity across the
   signer/transport/dispatcher; a valid signature does not prove the
   operator actually saw that string.

## CI, deployment and safety

Fedora 44 standalone tests compile with C++17 `-Werror` and OpenSSL
Ed25519; the fixture signs only synthetic messages. Tests cover
exact-byte equality, composed/decomposed UTF-8, wrong key/actor/decision,
changed message/revision/speaker/kind/challenge/nonce, corrupted
signatures, expired challenges, malformed/oversized UTF-8 and
**rejection of an old v1 proposal-only signature**.

The code is not linked into `main.cpp`, `Makefile.am`,
`Containerfile` or any production runtime. It has no SQLite writer,
grant/lease transition, service call, network, B10, Podman, provider,
Task or completion ACK. A successful CI or merge is **not** deployment.

Last operator-provided Fedora #274 proof concerned an isolated copy
and showed `evidence_consistent_no_authority`: original human
sequence 4 held at cursor 3, no submitted retry Task or ACK, provider
count 10, `Network=none`, provider OFF. This code does not assert
any fresher host state.
