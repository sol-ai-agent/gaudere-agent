# ADR: independent host trust controller boundary v1

**Status: proposed architecture, documentation only. No implementation or deployment approval.**

**Owner:** Sol (Gaudere core and trust contracts). **Out of scope:** Max's
B10 tooling/parallel rollout, Dev's Podman maintenance, or enabling any
provider/responder retry. This ADR requires a separate design/security review
before a host daemon, key, enrollment or Fedora service can be created.

## Decision and missing proof

PRs #286–#292 establish deliberately **non-authoritative** predicates:

- #286/#289: protected local root-pin and root-owned manifest inspection;
- #288: offline-root-signed ACTIVE/REVOKED policy and a local generation floor;
- #290: a signed host-receipt consistency check;
- #291: signed short-lived challenge-to-request/receipt binding;
- #292: one-use SQLite challenge CAS after cryptographic checks.

These do **not** authenticate where their allegedly independent inputs came
from. A caller can supply its own host public key, fingerprint, context,
clock, anchor epoch and SQLite file; ordinary Unix root ownership observed
**inside a rootless/mount namespace is forgeable**. No number of hashes,
signatures and local SQLite transactions can derive a real human operator
consent or genuine Fedora-host provenance from a self-chosen trust root.

**Decision:** Before production, place host identity establishment, trusted
key selection, challenge issuance and the ledger on a *separately secured
host trust controller* with a separately authenticated client/channel. Its
trust root must originate **outside the caller's writable trust domain**.
A client is not allowed to declare that its own inputs are independent.
This service has not been implemented, requested for deployment or enrolled.

If no externally protected trust root and authentic operator channel are
available, return **not_enrolled / host_trust_unavailable** and preserve all
other state. Do not downgrade to an all-caller-supplied "local trusted" mode.

## Trust boundaries and responsibilities

| Actor | May do | May never implicitly do |
| --- | --- | --- |
| Human operator at a separately authenticated local ceremony | Confirm intended trust-root fingerprint and scoped operation after reviewing its exact content | Be inferred from a reply message, Drive, model output, git merge or valid signature alone |
| Offline trust root | Sign operator ACTIVE/REVOKED policy and generation | Sign host attestation, run in container or delegate automatic enrollment |
| Exact-message operator signer | Sign exact, separately reviewed human intervention | Enroll itself, choose host verifier key or authorize an autonomous action |
| Protected host trust controller (future) | Hold host attestor key, independently pinned policy, issue random challenge and consume once under durable ledger | Let callers upload a new trusted host key, choose the root of trust, reset monotonic state, silently sign approval |
| Unprivileged client / rootless Podman worker | Submit bounded untrusted candidate and exact request context, receive read-only evidence | Supply host/key/floor/clock trust as authoritative, mint its own accepted nonce or cause execution from consistency success |
| Sol / Max / Dev GitHub/Drive workflows | Propose and test source and deployment changes with review | Count a PR, Drive content or B10 command as physical operator enrollment or a host-side source of truth |

A potential deployment could use a **system-level service** with its key and
policy in root/dedicated-service-owned storage, and a narrowly scoped Unix
socket. This is only a candidate, not a commitment: systemd system units,
Polkit/pkexec, service-user privileges, socket exposure, TPM/remote witness
and user-session behavior need a separate implementation/security design.

**SO_PEERCRED reports a process's kernel credentials, not the identity of a
person.** An attacker running as the same Unix UID, or inside a container to
which that socket was exposed, might be able to call the service. Restricting
the socket to UID 1000 or trusting `geteuid()==0` in a user namespace does
not establish human presence or application integrity.

A host-signed receipt proves only possession/use of a host private key.
Even if the signer is genuinely host-held, a permissive signing endpoint
would be an untrusted-caller signing oracle. The controller must enforce
explicit purpose, request provenance, rate/lifetime bounds and policy
**before** signing, and must never output an "approved intervention" solely
because the cryptographic receipt was consistent.

## Authenticated enrollment ceremony — future, explicit user action

1. **Unenrolled state:** no key, trust policy or ledger can be silently
   accepted from CLI arguments, Drive, a caller process, B10 or GitHub.
   No automatic key generation, bootstrap or repair on ordinary startup.
2. A future **human-initiated**, independently authenticated local action
   starts enrollment. It is not triggered by incoming chat, worker traffic,
   scheduled automation, agent restart or failed signature.
3. Present exact key *purpose*, public bytes/fingerprint, service identity,
   root-pin location, host context, intended operations and recovery policy
   **via a channel not controlled by the candidate key/Drive/request**.
4. Require a fresh affirmative operator confirmation after comparison to an
   independently obtained intended root identity. The same process may not
   supply the candidate key *and* the verifier fingerprint and claim success.
5. Provision host attestor private key separately from offline root and
   exact-message operator keys; preserve independently audited custody and
   rotation/revocation. Never place private keys in Git, Drive, B10, worker
   environment, argv, stdout, PR logs or test fixtures.
6. Persist a specifically scoped, versioned enrollment record and an
   externally verifiable checkpoint; on ambiguous commit **fail closed**.
   Previous records cannot be silently overwritten, reset or re-enrolled.
7. After enrollment, the controller can report a *read-only authenticated
   host identity proof*. A successful identity check is **not** itself an
   authorization or consent to execute a task.

Every operation that grants provider, dispatch, Task or ACK authority
requires another explicit reviewed gate, separate from this enrollment.

## Challenge issuance, signature and one-use consumption

A future controller, rather than an untrusted client, must:

1. Authenticate its own host enrollment **before** accepting a challenge
   request; identify the requesting peer, its actual scope and the exact
   purpose. For same-UID clients, define an independent authorization
   mechanism: UID-only is insufficient.
2. Draw at least **256 bits from a trusted CSPRNG**, inside the service.
   Compute the request context from immutable canonical operation/peer/
   version information, rather than accepting arbitrary "expected" hashes
   from the request. The nonce/challenge must be unique for the scoped
   host identity and never recycled after restart.
3. Atomically persist a pre-issued, unconsumed challenge bound to
   independently trusted host context, anchor epoch, exact request hash
   and bounded expiry **before** returning it. Do not expose uncommitted
   challenges as issued.
4. Respond over an authenticated transport; emit the #290 exact v1 receipt
   and #291 v2 proof with the appropriate separate purpose domains. Do not
   sign a foreign/caller root, context or epoch.
5. On consumption, validate #291 again *against controller-owned
   enrolled inputs*, then mark only the correct pre-issued nonce used once
   under #292's compare-and-swap. Two simultaneous clients must never both
   consume. An ambiguous COMMIT is **unknown, manual review required**,
   with no automatic retry or effect inferred from a missing response.
6. A consumed challenge can only evidence a bounded, one-time protocol
   exchange. Do not treat it as **operator approval** or automatically
   issue a Task/ACK/provider call.
7. Separate telemetry/status must distinguish `issued`, `consumed`,
   `expired`, `conflict`, `unknown_commit` and `not_enrolled`,
   without leaking tokens, private keys or exact sensitive text.

The current #292 SQLite DB is **local accounting only**. It offers serial
consistency and normal-restart persistence under its storage assumptions,
not a valid production challenge issuer or immunity to backup rollback.

## Rollback, time and namespace limitations

The host identity and monotonic generation ledger need a checkpoint whose
authenticity and monotonicity do not roll back with the same filesystem
snapshot. TPM-backed state or a separate authenticated witness is a
potential *future* option; no TPM support or rollback claim is assumed.
On a restored/unknown checkpoint, lost witness, clock uncertainty, issuer
rotation or split-brain, deny operations and require explicit operator
recovery. An operator can choose to change roots only via a new ceremony.

Host-root compromise, firmware compromise, a hostile system service with
the genuine attestor key, or a compromised authenticated operator channel
are **out of scope** for a pure software-local cryptographic verifier.
Security claims must state these limitations instead of calling the
result "host attestation" without qualification.

For Fedora rootless Podman, never accept a claim based only on:
`/etc` ownership as observed in the container, `geteuid`, an arbitrary
mounted Unix socket, an untrusted CA/public key read from the same image,
a rootless local SQLite directory, or matching fingerprints both
provided by the candidate. These can all be controlled together.

## Concrete negative acceptance tests required before implementation

| Case | Required outcome |
| --- | --- |
| No independently enrolled host key | `not_enrolled`, no nonce issued |
| Attacker mounts fake `/etc` or uses user namespace UID 0 | Deny as untrusted provenance |
| Caller supplies its own signing key, matching fingerprint and context | No authenticated-host result |
| Same UID connects to exposed local socket without separate authorization | No privileged signing/approval |
| Foreign root or operator key, wrong exact request, wrong epoch | Deny without consumption |
| V2 receipt/challenge replay on same DB, after normal restart | Exactly one local consumption, never a grant |
| Two processes race to consume one challenge | At most one durable `consumed` state |
| Lost reply after COMMIT | Unknown/manual review; no automatic retry or side effect |
| Clock rollback/expiry ambiguity | Deny, preserve evidence |
| Whole host DB + policy snapshot restored to older state | Deny **only if separately trusted checkpoint exists**; otherwise do not claim rollback resistance |
| Root rotation without independently authenticated operator ceremony | Deny |
| Real host signer willingly signs unapproved arbitrary caller request | Signing oracle design fails; reject controller design |
| Valid host challenge but no per-action human consent | No Task, ACK, dispatch, lease, provider or B10 effect |
| Max's B10 parallel status turns v1 stale | Do not mistake historical Drive status for current host trust evidence |

## Decomposition, owners and activation gates

1. **Sol, architecture:** review this ADR, define immutable wire context,
   host verifier provenance, authentication threat model, crash and recovery
   states. No executable host controller in this PR.
2. **Sol, source-only test stage:** disposable Fedora44 fixtures with a
   *separately mocked* trusted host authority; include malicious
   caller-supplied key/context, forged namespace, concurrent consume and
   restart/rollback negative cases. Any simulated admin/root keys are
   ephemeral and are never installed on Bertrand's Fedora.
3. **Human gate:** independently approve candidate identity source,
   operator ceremony, key custody, trusted time and rollback design.
   Without these approvals, no production trust service can be installed.
4. **Host service implementation:** a separate, explicitly authorized
   deployment and security review, coordinated with Max only if it touches
   his peripheral tooling. Never silently reuse B10 authorization.
5. **Live proof:** independent read-only host identity and challenge
   verification with negative adversarial tests. Actual operator
   enrollment requires another explicit confirmation.
6. **Authority gates:** any retry, provider, Task, ACK, dispatch or
   intervention is a *different project stage* requiring its own
   human approval and exact runtime evidence.

**No changes to** `gaudere-agent.service`, `gaudere-b10-*`,
Podman, the historical held sequence 4, old cursor, task ledgers,
network policy, provider state, or human-root enrollment are allowed by
this ADR. GitHub CI success for a design document does not constitute
Fedora deployment approval.
