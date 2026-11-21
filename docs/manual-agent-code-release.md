# Manual, code-only gaudere-agent release on Fedora

A pinned replacement for one-off manual release snippets, *not* a command
executor and *not* a B10 repair. No Task, provider call, ACK, model turn or
schema migration is authorized. This procedure is deliberately limited to the
known non-networked rootless Quadlet profile:

- `~/.config/containers/systemd/gaudere-agent.container`, mode `0600`;
- `Network=none` and volume `%h/.local/share/gaudere/state:/var/lib/gaudere:Z`;
- `gaudere-agent.service` initially active and backed by an immutable old
  image ID;
- candidate image already built from a clean, pinned checkout by
  `scripts/build-image.sh`, with `image_provenance=PASS`.

The script **fails closed** if the profile differs. The older, Stage9J6-specific
`scripts/deploy-gaudere-chat-code-production.sh` is *not* an appropriate general
release entrypoint: its hardcoded predecessor, provider and Task count invariants
may be obsolete.

## Operator procedure

Verify the actual running image ID, source checkout Git SHA and `gaudere.ref`.
Do not use a tag instead of a full immutable `sha256:<64 hex>` image ID. Export
four pinned values from independently verified evidence (the placeholders are
**not executable**):

```
GAUDERE_MANUAL_OLD_IMAGE=sha256:<observed-old-image-id>
GAUDERE_MANUAL_NEW_IMAGE=sha256:<proven-candidate-image-id>
GAUDERE_MANUAL_AGENT_REF=<pinned-agent-commit>
GAUDERE_MANUAL_CORE_REF=<pinned-gaudere-core-commit>
```

With all four values exported, execute `bash scripts/deploy-agent-code-only-manual.sh`
for a **non-disruptive preflight**. It checks the active service and Quadlet,
source provenance, exact candidate image labels, and `gaudere-chat --help` in
an isolated, no-network, read-only disposable container. Expect
`RELEASE=PRECHECK_PASS`. It does **not** stop or restart production.

Only after an explicitly approved maintenance window, set
`GAUDERE_MANUAL_RELEASE_ACK=APPLY_GAUDERE_AGENT_CODE_ONLY` and execute
`bash scripts/deploy-agent-code-only-manual.sh --apply`. The script acquires a
private non-blocking release lock, copies the original Quadlet, prepares a
candidate differing only in `Image=`, stops the agent service, creates a state
backup with the existing SQLite lock, verifies SHA-256, swaps the Quadlet image
atomically, reloads systemd, starts the candidate, verifies active image and
logical `.db` dumps have not changed, and retains the old image for rollback.

**Forward stop safety:** before any state backup or live Quadlet replacement,
require the old agent unit to be inactive and independently confirm its Podman
container is stopped/exited or provably absent (exact `podman container exists`
exit code `1`). An active/paused container, uninspectable existing container,
Podman store error, or failed stop command leaves the original Quadlet and data
intact and reports `ROLLBACK=MANUAL_REVIEW_REQUIRED`. The script does not issue
another stop or attempt a blind restart in this uncertain state. Inspect the
original service/container before deciding how to recover. This is a local
stop-state observation, not exclusion against unrelated external operators.

**Rollback safety:** a failed release never assumes that a successful
`systemctl --user stop` response means the candidate is quiescent. The
rollback path requires the unit to be inactive and the Podman container to
be stopped/exited or provably absent (`podman container exists` returns exactly
`1`). An active/paused candidate or Podman storage error (`125`) produces
`ROLLBACK=MANUAL_REVIEW_REQUIRED` **before** modifying the Quadlet or
restoring the SQLite state. Keep the failed state and backup for inspection;
do not relaunch blindly.
**Do not remove either the old Podman image or the backup.** `RELEASE=PASS`
means the internal release checks passed. `ROLLBACK=MANUAL_REVIEW_REQUIRED`
requires the operator to stop and inspect the preserved workspace; never blindly
retry an unknown-effect transition. This script does not replace a separate
post-deployment host inspection or permission to activate new capabilities.

No part of this procedure touches the B10 gateway or its Drive mailboxes.
