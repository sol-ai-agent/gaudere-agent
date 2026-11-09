# Responder retry generation 1 — isolated one-time challenge ledger

Status: **code-only, not deployed, no authorization**. This tranche adds a
versioned, independent SQLite file for tracking retry challenge reservations.
It neither replaces the historical v3 responder decision/disposition nor opens
or mutates the existing retry audit-lineage/scope sidecars.

## Why a durable ledger is needed

PR #277's Ed25519 signature inspection is deliberately **stateless**:
verifying the same signed bytes twice proves no new consent. The pure
signature predicate does not enroll an operator public key, generate a
cryptographically random nonce, authenticate the human or prove freshness.
A future trusted operator boundary must therefore allocate challenges
independently, bind them to the exact proposal/scope/lineage and atomically
record **one-time use** before any effect.

This tranche is **not** that boundary. Its caller-supplied challenge IDs,
nonce SHA-256 and evidence SHA-256 are **opaque, untrusted metadata**.
No method verifies a signature, proves that the nonce was randomly generated,
or decides whether a user actually approved anything.

## Isolated SQLite contract

`LocalGooseDialogueResponderRetryChallengeLedger` is a new file, schema v1:

- A canonical immutable reservation hashes request ID, exact v1 operator
  challenge ID, prepared grant-proposal ID, scope ID, lineage ID, nonce SHA-256,
  generation 1 and bounded issue/expiry time (maximum 5 minutes).
- Unique constraints on `request_id`, `challenge_id`, `nonce_sha256`
  and `(scope_id,retry_generation)` prohibit duplicate nonces and another
  challenge for the same generation-1 scope, even if challenge ID changes.
  This file does **not** authorize another scope or generation.
- Three states are supported: `reserved` (no evidence), `burned` (opaque
  evidence hash), and `manual_review` (uncertain/abort). Both `burned` and
  `manual_review` are **absorbing**: the API exposes no unburn, reset, delete,
  retry or transition back to `reserved`.
- `reserve()` accepts one new exact reservation or an
  `exact_duplicate_reserved` replay while it is still pending; any changed
  identity or prior burned/uncertain record is a conflict.
- `burn(challenge_id,request_id,evidence_sha256)` accepts exactly once
  under `BEGIN IMMEDIATE`; repeated calls return conflict **even when all
  inputs match**. A successful burn only irreversibly records one use:
  **it is not proof of consent, a grant or permission to dispatch**.
- `mark_uncertain()` moves only a reserved record into absorbing manual
  review. No TTL, missing heartbeat or restart makes it available again.

Creation is explicit and requires a previously absent regular current-owner
0600 file using `O_EXCL | O_NOFOLLOW`. Existing read-only and writer modes
cannot create files. Open rejects symlinks, unknown owner/mode, any SQLite
`-wal`, `-shm`, `-journal` companion, unknown/future schemas, foreign
tables or unsupported journal mode. SQLite itself opens with
`SQLITE_OPEN_NOFOLLOW`, writers use `synchronous=FULL` and
`BEGIN IMMEDIATE` for atomic reserve/burn/manual-review changes. The
file uses DELETE journal; concurrent processes serialize through SQLite,
and errors fail closed. A half-created file or ambiguous interrupted
transaction requires explicit operator inspection, never automatic repair.

## What it does NOT establish

This new store is not connected to PR #277 or any production runtime. A future
trusted source must supply a genuinely random nonce and **verify that its
hash matches the challenge** before calling the ledger. It must authenticate
a separately enrolled public key and the exact signed proposal, including
human-visible final intervention text (not covered by PR #277 yet). It must
re-check live historical evidence (cursor3, held human completion sequence4,
head revision3, expired historical lease/decision, no submitted Task) and
atomically enforce one-time use relative to any activation transaction.
Simply writing `burned` is not a grant, proof of consent or acknowledgment.
Crash boundaries **between this SQLite and future authority stores** remain
a separately designed and proven problem. No distributed transaction is claimed.

The synthetic Fedora44 CI verifies unique key collisions, exact replay,
burn-once across reopen, absorbing uncertain, read-only nonmutation,
missing/unsafe/linked files, companions, forged or future schema, generation2
and malformed fields. It does not prove a real host rollback or deployment.

## Production boundary

No `main.cpp`, `Makefile.am`, `Containerfile`, dispatcher, TaskStore,
provider/OpenAI, systemd, Podman, B10 or feed-ACK coupling. This tranche makes
no Fedora operation and creates no production sidecar. A CI-green PR/merge
cannot be used as an authorization or as evidence of a live operation.

Future gates: combine separately enrolled operator keys and signed exact
message, trusted challenge issuance, durable anti-replay and fresh evidence
into one reviewed typed authority service; prove crash/restart and concurrency
on synthetic and isolated Fedora copies; only then seek new explicit human
deployment/activation gates. The one-generation v1 retry budget does not
renew on timeout or failure.

Last real Fedora proof remains #274's user-provided frozen-copy PASS:
provider OFF, `Network=none`, 10 providers, original sequence 4 unACKed,
consumer cursor 3 and no retry Task.
