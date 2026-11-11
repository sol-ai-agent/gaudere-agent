# Retry host challenge-bound proof v2 — consistency only, no authority

**Source-only. No host operation, no real enrollment, no runtime wiring.**

#290 verifies a domain-separated Ed25519 host receipt bound to the observed
root anchor, host context and epoch. That receipt may remain valid for up to
31 days; it is **not a single-request or presence proof**. This v2 stage
provides a second host signature over the *exact v1 receipt hash*, a proposed
fresh challenge hash, a request context hash, and a short validity window.

## Exact canonical format

Canonical ASCII-printable and LF only; max 1024 bytes; exact order:

```text
gaudere-retry-host-challenge-proof.v2
receipt_sha256=<64 lowercase hex>
challenge_sha256=<64 lowercase hex>
request_context_sha256=<64 lowercase hex>
not_before_ms=3000
not_after_ms=180000
signature_hex=<128 lowercase Ed25519 hex>
```

Signature covers the domain string
`gaudere-retry-host-challenge-proof-signature.v2|`
followed by the exact bytes from the header through `not_after_ms=...\n`,
excluding the signature field. All SHA-256 strings must be canonical nonzero
lowercase, timestamps positive canonical decimal, interval positive with a
maximum length of **300000 ms (5 minutes)**. The window is half-open,
`not_before_ms <= trusted_now_ms < not_after_ms`.

## Preconditions and check order

1. Future independently authenticated caller supplies a truly trusted
   expected host public key+fingerprint, host deployment context,
   operator-root manifest, minimum anchor epoch, challenge and request
   context hashes, and a trusted clock. This implementation **does not
   establish their provenance**.
2. The exact v1 receipt is reverified against the #290 signature, the
   observed anchor/context, epoch floor and current time. Its bytes are
   SHA-256 hashed in their entirety, including its signature.
3. Parse the v2 canonical bytes, verify the **same separately pinned host
   Ed25519 signer** and the v2 signing domain.
4. Verify exact v1 receipt hash, independently expected challenge and
   request context, and the short validity window.
5. Report only
   `challenge_bound_cryptographic_consistency_no_authority`, or a
   specific rejection code. No Task, dispatch, ACK, lease, consent, root
   installation or operator enrollment is triggered.

## Critical limits

**This is not replay protection.** Repeating the same proof and the same
expected challenge yields the same no-authority result. The new v2 signature
means it would *fail* against a **different** expected challenge, but this
code does not mint a challenge, decide whether it is fresh, store or consume
one, identify the real host, or authenticate any independent expected
inputs. A malicious caller who sets its own host public key and challenge
can still self-validate without gaining legitimate authority.

Actual prevention of replay requires an independently authenticated host
service, unguessable challenge issued by a trusted client, context-bound
authenticated channel, **durable atomic one-use challenge consumption**,
timeout and failure recovery. No consumption ledger is included in this PR.
Rootless/mount-namespace spoof of root-owned files, same-host backup restore,
clock spoof, key rotation, root enrollment and genuine human presence remain
unresolved. A signature only proves possession of a signer key, not the
person who consented.

## Isolated proof matrix

Fedora44 C++17 `-Werror` synthetic tests verify:

- Canonical v1 and v2 host signatures and repeated no-authority output
- Wrong challenge, request context, another exact v1 receipt, higher epoch
- Foreign host signer, bad/tampered signature, invalid base v1 receipt
- Expired and premature proof, stale v1 floor, invalid expectations
- Bad ASCII/CRLF/TAB/UTF-8, missing/excess lines, incorrect signature
  domain, noncanonical decimals, invalid/overlong interval, oversize and
  all-zero signature

CI checks no main/Makefile/Containerfile integration, no on-host file
writes, private key generation, provider, B10, Task, ACK or dispatch in
production source.

## Next explicit gate

Before any deployment or operational trust: define authenticated **host
enrollment and attestor key provenance** independent of the container,
requests, Drive or local spoofed `/etc`; define a separate trust-preserving
challenge mint/consume protocol with durable anti-rollback and human
authorization. Only a separately approved operator action can initiate
actual keys or host installation. Nothing in #291 authorizes these effects.
Provider remains OFF, sequence 4 held, no ACK/Task.
