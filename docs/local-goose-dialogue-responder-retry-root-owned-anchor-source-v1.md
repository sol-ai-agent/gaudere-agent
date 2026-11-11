# Retry enrollment trust-source boundary v1: fixed root-owned manifest, no authority

**Source-only, no production wiring, no operator enrollment performed.**

The prior #286 root-pin reader checked a *caller-provided* path and trusted
SHA-256; #288 composed it with an independently supplied operator-key
fingerprint and existing read-only revocation floor. A caller still could
choose a new root/key and their own matching purported fingerprints.

This step introduces an explicit **fixed OS-controlled configuration source**:
`/etc/gaudere/retry-operator-root-anchor.v1`. The public manifest is *not*
a secret. The reader takes **no caller-supplied path, trusted root fingerprint,
operator-key fingerprint, or trust-store directory**. Its path is compiled
into the isolated C++ module; it cannot be changed by a Drive message,
candidate signed policy, Task, HTTP request or environment variable.

## Strict local file predicate

When invoked under the intended **non-root runtime UID**, the reader walks
`/`, `/etc` and `/etc/gaudere` using read-only, directory-relative
`openat`, `O_NOFOLLOW`, `O_CLOEXEC`, `fstat`, and root ownership with
no group/other write permission. It opens a single regular root-owned
`0644`/one-hardlink file, checks a bounded byte count and reads it exactly,
then compares file identity, size, `mtime` and `ctime` before/after the
read. Executing as UID 0 is refused to avoid treating a root process's own
file as independently enrolled evidence.

Exact canonical LF-only public format (all fields required in order):

```text
gaudere-retry-root-owned-anchor.v1
root_pin_path=/absolute/private/path/root.pin
root_fingerprint=<64 lowercase sha256 hex>
operator_key_fingerprint=<64 lowercase sha256 hex>
anchor_epoch=1
```

The path is syntactically canonical; the existing #286
`load_local_goose_dialogue_responder_retry_root_pin` separately validates
every path component, private owner-0700 immediate parent, owner-0400 file,
key format, link count and fingerprint. The unsigned `anchor_epoch` is
bookkeeping, **not** a revocation floor or anti-rollback primitive.

The resulting read-only inspector composes that protected root-pin handle,
#283 root-signed trust policy and #288 `existing_read_only` SQLite floor.
Results are `consistent_active_no_authority`,
`consistent_revoked_no_authority`, or explicit no-authority rejection.
No conversion of a success into permission is present.

## Boundary, provenance and unsolved threats

A file being root-owned only proves it was accessible under OS filesystem
permissions; **it does not prove a human approved installation**. A privileged
administrator or compromised root could install an attacker-selected root.
Actual first-time enrollment still requires a *separately authenticated
human-controlled local ceremony*, an independently displayed root fingerprint,
confirmation of intended root and operator key, atomic root-owned manifest
installation and a separate operator-visible audit. **These are not
implemented here.** No production file is created or changed by this PR.

A compromised runtime UID must not be allowed to own or rewrite `/etc`
trust configuration; the correctness of Linux ownership, mount namespace,
UID separation, and the deployed binary's build provenance are assumptions.
Privilege escalation, a malicious root or malicious image remain out of
scope. A **rootless container or attacker-created user/mount namespace can
present its own root-owned /etc tree**: `st_uid == 0` and a non-root
`geteuid()` inside that namespace do not by themselves establish *host*
root custody. In particular, the synthetic CI container deliberately proves
only filesystem predicates, **not** host enrollment provenance. Deployment
must refuse any container/runtime namespace whose mount trust cannot be
independently established, or must obtain the anchor through a separately
authenticated host-side service. Neither model is selected or implemented
here; this stage MUST NOT be promoted to runtime authorization.

Whole-host restore could roll back both manifest and SQLite;
external monotonic evidence is still required. The external clock is still
untrusted. A newer ACTIVE root-signed snapshot is **not** proof of a renewed
operator ceremony or permission.

A root-owned file alone must never count as human presence, approval for an
exact message, or a signing authorization. The private root signing key must
never be written to this manifest or repository. The public test-only
deterministic Ed25519 fixture is synthetic; no real operator key is involved.

## Synthetic proof and protections

CI executes only inside an **ephemeral Fedora 44 container**, as root to
prepare deliberately disposable `/etc/gaudere` fixture files, then forks
children to `setgid/setuid(65534)` for actual reader tests. The synthetic
test refuses any preexisting `/etc/gaudere` directory to avoid overwriting
state. It proves:

- Missing manifest, root-owned but writable file, writable parent, symlink
  and invalid self-claiming fingerprint are rejected.
- A valid public root-owned manifest is readable only from the fixed
  pathname. A lower-privileged caller cannot select an alternate root identity
  by injecting parameters because the API has none.
- Strict private root-pin matching, root-signed ACTIVE/REVOKED policy,
  foreign-root signature refusal, wrong expected operator key, tamper and
  expiry fail closed.
- The existing read-only floor refuses mutable ledger, then signed REVOKED
  generation 8 blocks an older ACTIVE generation 1 after reopening the DB.
- A newer ACTIVE can be *consistent but no-authority*; runtime grants and
  actual reenrollment remain separate gates.
- C++17 Fedora44 `-Werror` and static checks ensure no `main.cpp`,
  `Makefile.am` or `Containerfile` integration, host effects or provider
  calls. No file is installed on a real Fedora host.

## Explicit future gates

1. Separate operator-controlled enrollment interface, likely with privileged
   helper and human authentication independent of B10/Drive/request input.
   Do not decide whether polkit, physical console or other mechanism works
   before a separate review and real environment threat model.
2. Durable, auditable, crash-safe root-owned manifest installation/replacement
   with independent fingerprint confirmation and no silent reset; source
   predicate alone is insufficient.
3. Trust-store/mount integrity, external monotonic anti-rollback, clock and
   key rotation/revocation. Test crashes, concurrent operators, privilege
   separation and hostile caller-controlled values.
4. CI, security review, exact-head merge; **new human gate** for actual
   enrollment and another separate gate for Fedora/runtime/effects.

Provider remains OFF, durable call counter unchanged, historical sequence 4
unacknowledged, responder cursor 3 and prior lease/intent expiration untouched.
