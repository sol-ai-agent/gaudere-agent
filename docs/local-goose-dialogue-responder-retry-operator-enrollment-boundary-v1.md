# Retry operator enrollment boundary v1 — design only, no authority

**Status:** Contract for a future, explicitly human-gated implementation.
This document does not enroll keys, create any trusted configuration, generate
signatures, deploy to Fedora, advance an SQLite trust floor, or authorize a
responder retry. Source-only code from #277–#286 remains inert and disconnected
from production runtime.

## Problem and concrete threat

#282 verifies an operator Ed25519 public-key pin against a fingerprint supplied
by its caller. #283 verifies a root-signed trust snapshot. #284 stores a
monotonic *local* minimum generation. #285 gates floor updates on a root-signed
REVOKED tombstone. #286 requires a strict protected local public-root file
handle for that update path.

These are useful cryptographic and filesystem predicates, but they do not prove
who selected the **root path**, the **expected root fingerprint**, the
**operator-key fingerprint**, or the **trusted clock**. A malicious caller can
still supply its own key, its own fingerprint, and a signed tombstone. A caller
that controls both a candidate key and the alleged independent trust anchor
must **not** acquire an enrolled identity.

## Actors and distinct trust material

- **Human operator:** the only party that may initiate and confirm a new
  enrollment or trust-root replacement. Review of the *exact* retry message
  and any future production activation are separate acts.
- **Root key:** an offline Ed25519 signing identity for operator trust
  snapshots (ACTIVE/REVOKED and generation). Its private material stays outside
  repository, Drive, B10, runtime, logs and automated test fixtures; public
  root bytes alone do not imply enrollment.
- **Operator key:** distinct Ed25519 signing identity for exact human-reviewed
  intervention statements. The root-signed trust policy may declare it active
  or revoked, but a signature never grants runtime authority by itself.
- **Trust anchor:** operator-confirmed expected root fingerprint and fixed
  local root-pin location, obtained from a provenance independent of the
  request, candidate key file, Drive, and untrusted caller arguments.
- **Floor anchor:** monotonic trust generation and transition evidence. Local
  SQLite CAS prevents ordinary concurrent regressions; it is **not** a defense
  against full rollback of the entire filesystem/backup.

## Proposed enrollment ceremony (not implemented)

1. **Pre-enrollment deny by default.** No trusted root or operator identity
   exists in production today. A runtime path requiring enrollment must return
   an explicit unavailable/not-enrolled result, with no fallback to an
   arbitrary caller key, a test key, or a Drive-provided fingerprint.
2. **Explicit operator initiation.** Enrollment must be started through a
   separately authenticated, local human-controlled channel. No inbound
   responder event, background job, assistant, B10 request or GitHub merge
   may silently initiate or confirm the ceremony.
3. **Key provenance.** Root private material must be prepared and retained
   under an operator-controlled offline procedure; the distinct operator
   signing key must have its own custody and revocation plan. No private key
   generation or display is part of this design stage.
4. **Independent fingerprint confirmation.** Present the exact root public
   fingerprint and key purpose through an operator-trusted channel *independent*
   of the candidate file or the protocol request. Require an affirmative,
   authenticated comparison with the intended root key. Comparing two
   values both supplied by the same process or Drive document is insufficient.
5. **Fixed configuration and fail-closed persistence.** An independently
   authenticated controller must durably bind the exact root-pin pathname,
   root fingerprint, operator-key fingerprint and policy revision. A
   candidate pin is read-only and must satisfy #286 path/owner/mode/link,
   canonical-format and exact-fingerprint rules. Configuration writes and
   replacement require separate human gates and crash-safe audit.
6. **Negative enrollment checks.** Missing configuration, altered path,
   changed key, wrong owner/permissions, symlink, stale revision, unsafe
   anchor provenance, untrusted time or ambiguous commit must fail closed,
   never self-heal by accepting a new candidate key or resetting the floor.
7. **Post-enrollment inspection only.** A successful source-level proof
   can report identity/snapshot/exact-message consistency; it must not create
   a grant, lease, decision, intent, Task, dispatch, ACK, or provider action.
   All those transitions need separate generation-aware contracts and gates.

The precise authenticated channel, configuration format, backup/restore
protection and any hardware-rooted monotonic evidence remain **open design
decisions**. They must be resolved and adversarially tested before an
implementation can honestly label itself an enrollment controller.

## Rotation, revocation and rollback rules

- Root rotation is a new explicit operator enrollment with its own independent
  fingerprint confirmation. A signed trust snapshot from the **old** root
  cannot automatically choose the **new** trust anchor.
- Operator-key rotation/revocation requires a valid independently trusted
  root-signed snapshot, exact operator-key fingerprint, bounded timestamps
  and generation greater than the separately trusted floor. REVOKED must
  stay terminal for that generation; no fallback to a stale ACTIVE snapshot.
- #285/#286's verified revocation floor update is necessary but not sufficient
  to claim actual user consent. An authenticated controller must own the
  eligible caller inputs and persistence policy.
- A restored backup that rolls both trust snapshot and floor backward must
  be rejected using external monotonic evidence (for example an appropriate
  hardware or separately secured operator checkpoint). Until such evidence
  is defined, **do not claim anti-rollback against whole-system restoration**.
- Ambiguous writes require manual review; never repeat a trust transition
  simply because the response/ACK is missing.

## Required proof matrix before any real enrollment

| Scenario | Required result |
| --- | --- |
| No enrollment / missing trusted anchor | Deny; no default trust |
| Caller chooses its own root path and SHA-256 | Deny as untrusted provenance |
| Public key changed after fingerprint confirmation | Deny |
| Valid signature from a foreign root | Deny |
| Same root, wrong operator key / wrong exact intervention | Deny |
| Valid older ACTIVE after higher-generation REVOKED | Deny |
| Full rollback of floor plus snapshot, without external checkpoint | Flag unsupported; no anti-rollback claim |
| Two simultaneous enrollment/revocation attempts | At most one confirmed transition, otherwise manual review |
| Crash before/during/after durable update | Deterministic old/new state or fail-closed manual review |
| Key rotation or revocation without fresh operator ceremony | Deny |
| Signature valid but absent current event eligibility | No authority |
| Proof valid but no explicit final human authorization | No Task, dispatch, ACK, provider or B10 effect |

## Separate future delivery gates

1. Review this **documentation-only** contract and its threat model.
2. Design an authenticated local controller and independent root/floor
   anchors; prove its trust inputs cannot be selected by an untrusted caller.
3. Implement and test enrollment in isolated synthetic Fedora 44 fixtures
   with negative provenance, concurrency, crash and restore tests.
4. CI green, security review and squash merge of the exact tested head.
5. Any real operator key enrollment: **new, explicit human gate**.
6. Any Fedora deployment/integration: separate guarded gate and real proof.
7. Any retry authority, exact-message approval, Task/ACK or provider effect:
   independent contracts, review, operator gates and production proofs.

**Non-goals:** generating keys, adding a CLI, storing secrets, creating an
enrollment default, loosening path checks, using B10/Drive as an identity
oracle, altering production SQLite, or consuming the held sequence-4 event.
Provider/OpenAI remains OFF; the responder cursor/head and the historical
expired authority must remain untouched.
