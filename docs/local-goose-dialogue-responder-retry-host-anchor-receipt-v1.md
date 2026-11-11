# Retry host enrollment receipt v1 — isolated Ed25519 consistency, no authority

**Status: source-only / test-only.** No enrollment ceremony, root-owned host
manifest installation, host helper, Fedora action, runtime activation, retry
authorization, Task, ACK or provider call is implemented.

## Why #289 alone is insufficient

#289 introduces a fixed root-owned public trust manifest and prevents the
new inspector API from directly accepting caller-selected root paths or root
fingerprints. But apparent UID 0 and even `geteuid()!=0` inside rootless
containers, user namespaces or spoofed mount namespaces **do not prove host
provenance**. A malicious caller may create a fake `/etc` and install a
self-chosen manifest. Root-only filesystem checks are necessary local
predicates, not proof of human consent or of host origin.

A future host-side authenticated service may issue a cryptographically
bound statement about an independently checked trust configuration. This
PR tests the **signature and exact binding predicate only**, with a second
Ed25519 signing identity distinct from the retry-trust root and the exact
message operator signer. Neither a host service nor an enrollment process
is introduced.

## Canonical signed receipt v1

One exact LF-delimited UTF-8/ASCII record, up to 1600 bytes. Fields must
appear in order, once, with no trailing fields, CR, NUL, noncanonical
decimal notation or uppercase hexadecimal:

```text
gaudere-retry-host-anchor-receipt.v1
root_pin_path=/absolute/private/root.pin
root_fingerprint=<64 lowercase sha256 hex>
operator_key_fingerprint=<64 lowercase sha256 hex>
anchor_epoch=7
host_context_sha256=<64 lowercase sha256 hex>
not_before_ms=1000
not_after_ms=100000
signature_hex=<128 lowercase ed25519 signature hex>
```

Ed25519 signs the exact byte string

`gaudere-retry-host-anchor-receipt-signature.v1|` + all lines above through
and including `not_after_ms=...\n`, excluding the `signature_hex` line.

Host signing key fingerprint:

`sha256("gaudere-retry-host-attestor-ed25519-key-v1|" + 32 public key bytes)`.

The receipt does not contain the host signing public key. The verifier
accepts an **externally pinned host public key**, matching expected host
signer fingerprint, **externally expected host context hash**, previously
observed root-owned manifest record, **separately trusted minimum anchor
epoch** and **externally trusted time**. It validates:

1. Canonical exact bytes/size/path/hex/decimal/interval; maximum signed
   lifetime 31 days.
2. Exact host Ed25519 key/fingerprint and signature with domain separation.
3. Root-pin path, root fingerprint, operator signing key fingerprint and
   anchor epoch exactly equal to the separately observed manifest.
4. Host context matches the *independently expected* deployment context.
5. The signed epoch is not below the independent minimum; `now` lies in
   the half-open signed validity interval.
6. On success return only
   `cryptographically_consistent_no_authority`, never a grant.

**Security-critical caveat:** this C++ API cannot verify that its "expected"
values were **actually supplied from independent host/operator provenance**.
If an untrusted caller supplies both its own host Ed25519 public key and a
matching fingerprint/context/epoch, it can self-validate its own receipt.
Tests show a hostile signer is rejected **only against a separately fixed
expected signer**. This source-level predicate MUST NOT be treated as host
attestation, operator enrollment, human approval or even evidence of real
host deployment. No code consumes its success as runtime authority.

Root-owned manifest read and signed receipt verification are separate
observations, not a cross-process atomic transaction. A future host helper
must address concurrent changes, namespace/channel binding, caller
authentication, replay resistance, trusted time, revocation, and external
anti-rollback before any claim of trustworthy provenance.

## Negative matrix — synthetic-only

The isolated C++17 Fedora44 strict-`-Werror` test generates **synthetic**
deterministic keypairs in test scope only and exercises correct
domain-signed receipt, repeatable no-authority result, unknown/foreign
self-signer, wrong independently pinned key/fingerprint, signed
tampering, root path/key/epoch mismatch, wrong context, old epoch, expired
time, invalid/trailing/reordered format, overlarge payload, bad epoch,
noncanonical path and wrong signature domain. No real host filesystem or
user data is touched; test never generates a production operator key.

The workflow also verifies this source is absent from main/Makefile.am/
Containerfile and contains no filesystem writes, privileged operations,
key generation, provider, Task/ACK, or dispatch.

## Explicit unresolved design and activation gates

- Authenticated host-side service identity, whose public verifier key cannot
  be selected by Drive, the request, a container-mounted `/etc`, or the
  verifying client. A correctly signed **fake** host claim is still fake
  if its signing key has no independently authenticated provenance.
- Real operator-initiated enrollment and purpose-specific human ceremony;
  separate audit, exact root fingerprint review and replacement/revocation.
- Host source/namespace pinning, filesystem races, transport authenticity,
  replay, independent monotonic epoch protection against full rollback,
  clock authentication, and key rollover. A signed receipt alone is not
  replay protection.
- Full CI, review and source-only merge first. Any actual key generation,
  enrollment or installation on Fedora requires explicit human approval.
  Any runtime grant/lease/decision/intent, Task/ACK/dispatch or provider
  operation requires further separate gates and live proof.

Provider remains OFF, held sequence 4 is not consumed, historical
responder cursor/lease/disposition are not modified. Max's B10 production
work and Dev's Podman cleanup are independent responsibilities.
