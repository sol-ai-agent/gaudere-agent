# Confined Fedora retry admission helper — v1

Status: **code-only, no host runner and no production authorization**.

## Goal

Provide a narrow executable that can inspect the exact recovered historical
responder authority chain and admit **one generation-1 retry lineage** only
inside a copied SQLite rehearsal workspace. This is not dispatch authority,
and the resulting lineage is not a grant, lease, decision, intent, Task or ACK.

The historical Fedora recovery of sequence 4 is already completed. Its
old grant/lease/decision/intent/disposition must never be reactivated or reused.

## Execution contract

The executable `gaudere-responder-retry-admission-isolated` has precisely:

- `--help`
- `inspect` — validate recovered evidence, read and report any retry lineage,\n  without creating an absent retry sidecar
- `admit-generation-1` — call the typed #266 admission service to append one
  immutable retry lineage; exact replay returns `duplicate`

All paths are **hard-locked** under `/rehearsal`. The five SQLite files are:

- `state.db` — authoritative Task and provider state
- `local-goose-dialogue-thread.db` — preferred head
- `local-goose-dialogue-completion.db` — completion feed and ACK
- `local-goose-dialogue-responder.db` — historical authority schema 3
- `local-goose-dialogue-responder-retry.db` — dedicated retry schema 1

The first four must already exist in a copied workspace. Only the fifth may
be created and appended by the explicit `admit-generation-1` command.
The `inspect` command does not create an absent retry file. No path flag, arbitrary command,
runtime submission or control socket is exposed.

An exact process replay uses the persisted lineage creation timestamp, so
`admit-generation-1` is idempotent even when the host clock advances.

## Preconditions

The helper and typed admission service jointly verify:

1. consumer `system-responder-v1` remains at cursor 3;
2. sequence 4 remains the exact pending human completion, unACKed;
3. preferred head remains revision 3 / exact triggering Task;
4. historical grant remains activated but its authorization window expired;
5. lease expired with zero turns committed;
6. decision expired with `bound_intent_expired_before_dispatch`;
7. intent expired with `lease_or_intent_expired`, no submission;
8. disposition `manual_review` with `lease_or_intent_expired`;
9. historical Task submission idempotency key has no durable Task;
10. exact historical IDs, event, hashes and head evidence still match.

The helper prints `GAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED=PASS`
only after the new lineage is persisted and the trigger invariants are re-read.
The inspector prints its own explicit PASS marker.

## Fedora proof runner is a separate future tranche

A host-side runner must be independently reviewed, CI-tested and merged before
any real Fedora proof. That runner must:

1. require a dedicated human authorization and clean exact `main`;
2. check rootless Podman, exact production image and core pin,
   `Network=none`, provider total 10 and dormant authority;
3. capture a stopped-state backup, make consistent SQLite backups into a
   private rehearsal directory, and restart the **unchanged** service;
4. mount **only the rehearsal directory** at `/rehearsal:rw` into a
   read-only, network-none, capability-dropped helper container;
5. run `inspect`, `admit-generation-1`, `inspect` and exact replay;
6. compare logical SQLite digests: state/thread/completion/responder must be
   unchanged; only rehearsal retry sidecar may be created or appended;
7. confirm no new authority, dispatch, Task, ACK, head advance, provider call
   or production retry sidecar exists;
8. fail closed on any unexpected evidence and preserve backup/workspace.

**This PR does not implement or execute that host runner.**
The CLI must not be run with production files bound to `/rehearsal`.

The later production admission of a real retry lineage, and the still-later
grant/lease/decision/intent/dispatch stages, each require **separate explicit
gates**. No step may implicitly advance authority.

## Validation boundary

The GitHub gate compiles the helper on Fedora 44 with the pinned core
and `-Werror`, checks the narrow CLI and scans for forbidden submission,
ACK, lease, grant and production wiring. It does not itself count as a
real Fedora workstation proof.

Historical sequence 4 remains pending and unACKed throughout this code-only
stage; provider/OpenAI remains OFF.
