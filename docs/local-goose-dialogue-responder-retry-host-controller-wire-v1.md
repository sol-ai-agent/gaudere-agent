# Host controller wire v1 — untrusted request, denial-only result

**Status:** Source-only C++17 and synthetic negative tests. No host controller, enrollment, authenticated transport, keys, Fedora service, Task/ACK, provider, B10 or live agent wiring.

This implements a *narrow* wire-input boundary of [ADR #293](local-goose-dialogue-responder-retry-host-controller-boundary-adr-v1.md). It does not resolve the missing independently protected host trust root or independently authenticated human presence. A syntactically valid client message is **never** host provenance or permission to act.

## Canonical untrusted request

Exact ASCII, LF-terminated, maximum 256 bytes, no CR, spaces, reordered/repeated or unknown fields:

```text
gaudere-retry-host-controller-intent.v1
operation=inspect_identity_read_only
request_context_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1
```

Only one additional operation token exists: `request_scoped_challenge`. This is a request addressed to a *future* issuer, not a challenge or approval. The 64-character nonzero lowercase SHA-256 is caller-provided input, not independently derived context. The parser never accepts a caller-selected `host_id`, public key, root/path/fingerprint, `uid=0`, namespace identity, host time, epoch, nonce, operator consent, provider or Task/ACK field. Any extra input is rejected.

## Denial-only v1 responses

```text
gaudere-retry-host-controller-denial.v1
status=not_enrolled
```

Exact other statuses: `peer_not_authorized`, `host_trust_unavailable`, `operator_required`, `clock_uncertain`, `commit_unknown`. Maximum 128 bytes. The last state means the commit outcome is uncertain and must be manually inspected: never retry as though nothing happened.

There is deliberately **no** `approved`, `verified`, `issued`, `consumed`, `enrolled`, `grant`, Task, ACK, provider or successful host-attestation result in this version. Even a well-formed denial delivered over an untrusted transport is not proof that a host controller said it.

## Security limits and gates

- Files are not linked to automake, main, container or host executor. There is no socket, filesystem read/write, clock, key, ledger, signer, nonce generation, runtime state transition or execution authority.
- Synthetic tests reject forged `/etc` claims, fake UID 0, caller-provided keys, clock/epoch/nonce and approvals. They **do not** simulate a real protected host identity; refusing extra fields is only a narrow input-contract property.
- A later stage requires a separately test-owned authority fixture with immutable trust root, peer provenance, trusted clock, issuance persistence, concurrent one-use CAS, restart/rollback and signing-oracle negative cases. Production service and operator enrollment require their own independent explicit approval.
- Valid parsing and the prior #290–#292 cryptographic consistency checks cannot themselves grant retry, Task, ACK, dispatch, provider or B10 actions.

## Validation

Fedora 44 CI builds and runs the standalone C++17 negative test, rejecting malformed header/version/order/length/hex/trailing data, spoofed host/operator fields and invented success replies. CI checks that the new parser is absent from production build and entry-point wiring.

