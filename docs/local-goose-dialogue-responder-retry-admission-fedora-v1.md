# Fedora isolated retry admission proof — bounded gate

Status: **code-only until merge + explicit Fedora authorization + real PASS**.

The canonical historical responder authority expired-bind was recovered in production
on 2026-10-29: original grant activated with expired window, lease expired with
0/1 turns, decision/intent expired, disposition manual_review, event sequence 4
unACKed at cursor 3 and head revision 3. This runner **must not change that**.

## Contract

Versioned runner:
`scripts/prove-responder-retry-admission-isolated-fedora.sh`.

Invocation is always by `sh` because the runner's repository mode is intentionally
nonexecutable. Requires explicit dedicated token:
`GAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED_AUTHORIZATION=AUTHORIZED_RESPONDER_RETRY_ADMISSION_ISOLATED`.

This token authorizes only a controlled **stop → stopped-state backup → four SQLite
snapshots → immediate restart → isolated rehearsal**. It is not authority to
prepare a real production retry, grant, lease, decision, intent or dispatch.

### Phase 1: fail-closed preflight

- exact clean `main == origin/main`; core pin unchanged;
- rootless Podman;
- live image exactly `93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071`;
- service active and `Network=none`;
- provider total 10;
- original responder/authority sidecar schema 3 and **no production retry sidecar**;
- consumer cursor 3, exact human event sequence 4 and preferred head revision 3;
- observer `intervention_owned`, disposition state 4, no automatic observer arm;
- default authority disabled, state/mutation surface absent.

Runner builds a tagged helper image from the pinned core, then checks image
provenance and the single `inspect|admit-generation-1` surface.

### Phase 2: immutable source snapshot

Stop `gaudere-agent.service` only for a backup via
`GAUDERE_STATE_DIR=... sh scripts/backup-state.sh` and
`sqlite3.Connection.backup` of the **four original SQLite files** into a
private rehearsal directory. Restart the unchanged service **before**
rehearsal. Failure while stopped triggers a best-effort restart and emits
`SERVICE_RECOVERY=restart_attempted_without_production_commit`.

No backup is restored automatically. No production state is copied back.
The production retry sidecar must stay absent.

### Phase 3: isolated admission

All helper invocations are Podman `--network none`, read-only image, no
extra capabilities, no new privileges; they mount **only**
`$workspace/rehearsal:/rehearsal:rw`.

Expected:
1. `inspect` passes and creates **no** retry sidecar;
2. `admit-generation-1` returns `accepted`, creating one lineage in the
   *rehearsal* retry sidecar;
3. `inspect` reports `LINEAGE_PRESENT=yes`;
4. same admission returns `duplicate`, not a second lineage;
5. retry schema 1 and exactly one lineage for the canonical event/consumer;
6. logical SHA-256 of the four copied pre-existing databases is unchanged;
7. live production still passes the exact prestate, with no retry sidecar.

A PASS proves *one retry lineage may be admitted in a copy*.
It does **not** prove the lineage exists in production and does not
allow any production authority or Task/ACK mutation.

Final expected marker:
`FEDORA_GAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED=PASS`.

Failure marker:
`FEDORA_GAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED=ABORTED`.

On failure, **inspect the exact phase before any rerun**. Save BACKUP and
TRANSITION_WORKSPACE paths; do not restore old state or resubmit historic
authority.

## Next required gates

After this isolated PASS, a **separate, specifically authorized** production
admission gate may be designed. A real lineage has no grant/lease authority.
A second separate design and proof would still be required to create a new
finite retry grant/lease, then another to decide/bind and dispatch. Never
reactivate historic grants/leases/decision/intent/disposition.

Provider/OpenAI remains OFF, cycle/stimulus dormant, and sequence 4 pending.
