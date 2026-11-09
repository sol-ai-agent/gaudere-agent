# Responder retry generation 1 — offline operator signature *inspection* v1

Status: **source-only cryptographic predicate; not a grant, permission, activation,
credential enrollment, challenge issuer, or live operator UI**.

This follows the inert retry grant proposal (PR #276) and scope store (#275).
The real Fedora #274 proof only showed `evidence_consistent_no_authority` on
a **frozen copy**. It did not authorize a retry or change production.

## Trust question addressed

The prepared-only proposal has an `approval_reference`, but that string may
be invented by a caller. Neither the proposal hash nor a timestamp proves
human authorization. This tranche implements a deliberately narrower,
standalone Ed25519 **signature inspection** over one immutable, exact
generation-1 proposal/scope, using a **separately pinned public key**.

The v1 inspector does not generate, enroll, rotate, distribute, infer or trust
a public key from the request, challenge, JSON, task, Drive, model, environment,
or user-supplied chat text. The caller must supply the key from a genuinely
trusted operator-enrollment boundary which **does not yet exist**. That future
enrollment must verify the user's identity/possession and protect the private
signing key in the operator's own device, separate from model-generated work.
An arbitrary caller choosing both a key and an approval signature must never
obtain authority.

A valid signature **proves possession of a key relative to the configured
public key**, not actual human presence, knowledge, intent, or current consent
without such enrollment and interactive confirmation.

## Domain and exact binding

`make_local_goose_dialogue_responder_retry_operator_challenge` takes a
canonical, prepared-only proposal and matching immutable scope; an explicit
256-bit lowercase-hex nonce; an issue time not preceding preparation; and an
expiration no later than the grant-proposal expiry and at most 5 minutes after
issue. The nonce is supplied by the future **trusted challenge issuer**, not
generated here. Entropy and unique challenge issuance cannot be proven by this
pure function.

The domain-separated SHA-256 challenge ID binds the exact proposal ID,
scope ID, lineage ID, approval reference, nonce, issued and expiry timestamps.
The signed Ed25519 payload additionally binds the challenge ID, exact observed
operator id `bertrand`, and the single decision literal
`approve_retry_generation_1`. No wildcard authority, new message content or
generic approval is permitted.

The inspector bounds all challenge identifiers and exact prefix/hash lengths before canonical hashing; oversized or malformed values fail closed. It rejects invalid source proposal/scope, altered challenge, wrong
nonce, stale/early/expired proof, wrong actor/decision/key fingerprint,
malformed signature or a signature for another challenge. It verifies against
one separately pinned Ed25519 public key. The result is explicitly named
`cryptographically_consistent_no_authority`, never `approved` or
`authorized`.

**No private key is present in production code.** The tests sign using a
synthetic fixed seed compiled only in the isolated CI executable. The fixed
seed MUST NOT be treated as an enrolled operator key.

## Security limits intentionally NOT closed

1. **Authenticated key enrollment:** a trusted, out-of-band operator ceremony
   must bind the public-key fingerprint to the real human and retain an audited
   rotation/revocation process. No hardcoded, user-supplied, model-supplied,
   implicitly imported or test key is accepted in a production design.
2. **Interactive consent:** the operator must see and confirm the exact
   proposal/lineage/sequence/head/speaker, the exact final intervention message
   and constraints. This proof only signs the *proposal*; it cannot authorize
   unspecified future message text, a hidden decision or a later Task.
3. **Durable anti-replay:** a separate transactional nonce/challenge ledger
   must atomically allocate a collision-resistant nonce and consume it once
   before any activation/effect. The inspector is intentionally stateless;
   repeated verification of the same signed bytes succeeds and is **not**
   one-time permission.
4. **Fresh state:** after a valid signature, a future typed trusted service
   must inspect the actual authoritative stores *again*, proving cursor 3,
   the exact unACKed human sequence 4, preferred head revision 3, historical
   terminal disposition/intent/decision/lease, valid lineage and no Task.
   Nothing may be inferred from the frozen proof, a replayed signature, or
   a scope record alone.
5. **Bounded authority and dispatch:** only a separately versioned,
   generation-aware grant/lease/decision/intent/disposition store and reviewed
   retry-aware dispatcher may stage or submit the one permitted intervention.
   Never alter historical responder schema-v3 records or create Task/ACK
   directly. Generation 2 requires a wholly new explicit gate.

## Isolation and evidence

New files: `LocalGooseDialogueResponderRetryOperatorProof.hpp/.cpp`, an
adversarial synthetic test and an isolated Fedora 44 CI workflow. The
implementation only uses SHA-256 and OpenSSL EVP Ed25519 verification; it does
not open SQLite, call the runtime, systemd, Podman, Drive, OpenAI or B10, or
perform network or host effects. No linkage to production `main.cpp`,
`Makefile.am` or `Containerfile`.

The CI's synthetic private key, while useful for proving signature-binding
and tamper rejection, **cannot prove operator enrollment or human approval**.
Neither CI nor merge are a Fedora deployment gate. There is no new authority,
provider call, system turn, Task, response or completion ACK.

Next: choose and review an explicit operator key-enrollment/signing UX; a
strictly isolated durable nonce-consumption ledger; exact-message human
confirmation and independent fresh eligibility; then only a separately
authorized Fedora canary. Until then, this inspector may not be used by an
execution path.

The last operator-reported Fedora state is unchanged: provider OFF,
`Network=none`, provider total 10, human sequence 4 held, cursor 3, no
retry submission Task or ACK.
