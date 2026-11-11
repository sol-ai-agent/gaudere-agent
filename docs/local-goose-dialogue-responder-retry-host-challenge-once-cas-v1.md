# Host challenge single-use ledger v1 — atomic local accounting, no authority

**Status:** Source-only C++17 and synthetic-only Fedora44 tests.
No production runtime wiring, B10/Podman task, private key, host operation,
operator enrollment, provider, ACK, or retry authority.

## Threat and precise scope

#290 verifies a proposed host-signed v1 receipt. #291 verifies a v2
challenge-bound signature for the exact receipt and request context with a
short validity window. But #291 alone still accepts the same proof when
repeated with the same expected challenge. This stage makes a narrow
**local one-use accounting assertion** possible by atomically marking an
already-issued challenge row as consumed.

A trusted host challenge issuer, genuine host identity, human-controlled
enrollment and authenticated channel **do not exist in this stage**.
The SQLite state is neither hardware monotonic nor protection against a
full rollback, copy, restore, same-UID hostile replacement or malicious
caller fabricating the complete trusted input set. Local one-use accounting
must NOT be described as complete replay protection or human consent.

## Storage shape and ownership

Open an **existing** SQLite file only, with `SQLITE_OPEN_READWRITE`,
`SQLITE_OPEN_NOFOLLOW`, private owner-0700 immediate parent, regular
owner-0600 one-link DB, strict schema, `PRAGMA user_version=1`,
`journal_mode=DELETE` and writer `synchronous=FULL`; reject WAL,
shared-memory and rollback-journal companions at open. No creation,
implicit bootstrap, reset, repair, or issued challenge is exposed through
the C++ class.

The exact schema has a single challenge SHA-256 primary key, expected
request-context SHA-256, expected host-context SHA-256, root anchor epoch,
expiry epoch milliseconds, `consumed` boolean and accepted exact v2 proof
SHA-256. The test bootstrap pre-seeds only disposable fixture rows.
A future **separately authenticated host controller** must determine how to
issue unpredictable challenges, durably record their context/expiry, prove
which application requested them, and authenticate the host signing key.
No such controller exists or is created by this PR.

## Verified commit contract

The only public mutation bridge:
`verify_and_mark_local_goose_dialogue_responder_retry_host_challenge_once`

1. Verify exact signed v1 host receipt and v2 challenge proof through #291,
   including host signer/fingerprint, anchor, epoch floor, challenge and
   request hashes, trusted time and signatures. A failed proof cannot
   consume a row.
2. Take a SQLite `BEGIN IMMEDIATE` lock; find the pre-issued challenge
   by its independently expected SHA-256; reject missing or consumed.
3. Compare row request, host context and anchor epoch against the verified
   inputs; reject expiry or mismatch. Do not take values from a fabricated
   v2 message as authoritative ledger data.
4. CAS-update exactly one row where `consumed=0` and all context/epoch/
   expiry constraints still hold; persist SHA-256 of the *exact* accepted
   v2 proof, then `COMMIT` with `synchronous=FULL`.
5. Report only
   `verified_challenge_marked_consumed_no_authority`, duplicate/missing,
   wrong context/expired, invalid proof, or commit-uncertain result.
   **Never retry an ambiguous commit automatically.**

Successful status does not issue a grant, lease, Task, dispatch, ACK,
provider request or consent. Reopening the DB after normal process restart
will not issue or reset consumed challenges.

## Synthetic adversarial matrix

Fedora44 isolated `-Werror` test verifies:

- Signed v1/v2 synthetic Ed25519 proof, successful single commit.
- Forged v2 signer rejected without consuming the challenge.
- Identical replay rejected both in process and after a fresh ledger open.
- Distinct challenge and request context enforced; expired issued challenge
  cannot be used, but legitimate earlier use remains possible.
- Two competing child processes, each with its own SQLite connection,
  simultaneously contend for one challenge: **exactly one** commit and
  one replay rejection.
- Missing database refuses to open without creating any file.
- CI static guard prohibits production runtime/wiring/privileged effects.

## Open problems and independent gates

The alleged independent host key and expected challenge in the bridge are
still caller inputs; unsigned local bootstrap fixtures are NOT proof of
proper issuance. Rootless mount/UID spoof, same-UID file replacement and
host rollback remain in the adversarial model and unresolved. A hostile
caller that installs a fake DB and supplies self-signed fake host material
can receive a local no-authority success. The database only protects
ordinary serializable competition and restart in a controlled file.

Before a real host challenge can safely be consumed operationally:

- Independently pinned host attestor public key from human-controlled
  enrollment; a real separately authenticated host process and trusted
  client identity.
- Cryptographically secure challenge issuance in that host process,
  channel/session binding, durable receipt of issue and atomic consumption
  scoped to a real user decision or narrowly defined operation.
- Filesystem/namespace integrity and independent monotonic anti-rollback
  against restored DB or cloned state, crash/power-loss and backup proof.
- Trusted clock and root/key rotation, operational recovery after ambiguous
  commit, user review of exact intervention message and separate consent.
- New CI/source review and explicit human gates for any Fedora deployment,
  key creation/enrollment, Task/ACK, provider or runtime authority.

No work is delegated to Max/B10 by this change. Provider remains OFF and
held sequence 4 is not consumed.
