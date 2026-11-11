# Local Goose retry — root-signed operator key lifecycle snapshot v1

**Code-only and inert**: No host deployment, no actual key enrollment, no
human approval, no runtime authorization, no retry dispatch, no Task/ACK.
This stage follows the source-only #282 local Ed25519 public-key pin.

## Why a separately pinned key file is not enough

The #282 inspection requires an independent SHA-256 fingerprint so that a
caller-chosen public key cannot validate its own signature. That is necessary
but insufficient for rotation, revocation or a compromised key. We also need
a signed trust-state snapshot and a way to reject older, valid signed ACTIVE
snapshots when a key has been revoked.

This stage adds one bounded, canonical UTF-8 document signed by a **separate
offline Ed25519 trust-root key**. It authenticates a local operator key
fingerprint and its status for one generation. The root public key, root
fingerprint, accepted minimum generation and clock **must be controlled
independently by a future operator enrollment mechanism**. The document
may be received from untrusted storage because its signature is checked,
but it must never be able to replace these independent trust anchors.

## Exact snapshot format

No JSON interpretation, whitespace normalization, duplicate or unordered
fields; exact LF line endings and lowercase hexadecimal:

```text
gaudere-retry-operator-trust-snapshot.v1
operator_id=bertrand
key_fingerprint=<64 lowercase SHA-256 hex>
status=active
generation=7
not_before_ms=1000
not_after_ms=30000
signature_hex=<128 lowercase Ed25519 hex>
```

The format permits `status=revoked` as a **signed tombstone**, but it
never authorizes a signature. The signature is Ed25519 over:

```text
gaudere-retry-operator-trust-snapshot-signature.v1|<raw first seven LF-terminated lines>
```

Fields are exactly ordered. Decimal values are nonnegative, minimal
(no leading zeros); generation starts at 1 and fits signed 64-bit;
timestamps are signed 64-bit milliseconds, strictly increasing, with
validity no more than 366 days. Overall snapshot length is at most 1024
bytes. The root fingerprint is computed with:

`SHA256("gaudere-retry-operator-root-ed25519-key-v1|" + 32 raw root public-key bytes)`

No root or operator **private key** may enter source control, Drive or B10.

## Verification sequence

1. Fail closed if the independently trusted root fingerprint does not
   match the separately supplied root key, or the minimum generation is 0.
2. Strictly parse the candidate document and validate the **root
   signature** over the exact status/fingerprint/generation/timing bytes.
3. Refuse `revoked` always, any signed older generation below the
   independently trusted generation floor, and any expired/not-yet-valid
   snapshot. A revoked generation can be stored as durable negative
   evidence, but this read-only API never writes or persists it.
4. On an active snapshot, use its **signed** key fingerprint to verify
   the separately stored owner-0400 public-key file via #282. Then verify
   the #279 Ed25519 signature on the exact UTF-8 intervention and full
   request scope. Return at most
   `cryptographically_consistent_no_authority`.

A signed ACTIVE generation 7 is rejected when independent durable state
advances its minimum generation to 8 (even if an attacker replays the
valid generation-7 bytes). A signed REVOKED generation 8 is rejected even
when the local key and message signature remain valid. If the trusted
generation floor is not advanced and the caller can replay old snapshots,
**revocation is not yet durable or effective**. A future operator-controlled
state store is therefore mandatory.

## Strict synthetic Fedora44 tests

Tests generate disposable deterministic root and operator Ed25519 keys in
memory, then verify:

- a root-signed ACTIVE snapshot and exact operator signature passes,
  with an explicit non-authority result;
- unauthenticated root substitution and incorrect independent root SHA-256
  cannot replace the root; zero minimum generation fails;
- a correctly root-signed REVOKED generation refuses the operator key;
  lowering/replaying old signed ACTIVE generations below a trusted
  higher floor fails;
- mutations to status/signature and malformed format, timing, decimals,
  duplicate/trailing records fail closed;
- signed key rotation requires a matching replacement of the local
  protected key pin and a fresh exact signed-message proof;
- bad local key permissions or a symlink defeat even an otherwise valid
  signed ACTIVE snapshot.

This test is isolated from the real Gaudere user runtime. CI checks that
`main.cpp`, `Makefile.am`, `Containerfile` do not reference the class.

## Trust gaps and next integration gates

These signatures are **synthetic test evidence**, not human consent.
This API accepts the root pubkey, its root fingerprint, generation floor
and clock as parameters. There is no way here to prove that their caller
is trustworthy, enrolled or independent from a request; a malicious
caller may pick its own root and fingerprint and thereby verify its
own policy. The intended trust boundary MUST be implemented later.

Required before an effective real-world retry: an authorized human
enrollment ceremony for root and operator keys, safe storage/rotation
and irrevocable generation floor updates, fresh source eligibility,
operator UX demonstrating display/approval of exact text, challenge
issuance with CSPRNG and trusted clock, grant/lease/Task/ACK transaction
handling, crash-safe manual review, and explicit Fedora activation
authorization. The #282 public-key file reader also requires trusted
directory ownership/modes in a production controller.

**No B10, WorkerDev, WorkerMax, Fedora, Podman, production credentials
or provider were accessed or changed by this PR.**
