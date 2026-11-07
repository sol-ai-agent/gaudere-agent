# Retry generation 1 — confined frozen-snapshot eligibility helper

Status: **code / CI only**. This tranche adds no Fedora runner, does not
authorize host access, and does not inspect production. It follows #269
(production audit lineage PASS reported 2026-11-06), #271 (const-store
eligibility) and #272 (existing-only retry SQLite read-only opener).

## Scope

A standalone `gaudere-responder-retry-frozen-snapshot` binary accepts only
`--help` and `inspect-generation-1`. Paths and expected lineage ID are
compiled in; there is no caller-supplied path, request ID, operation or
authority object.

Five files are expected **only** in `/rehearsal`:

- `state.db`
- `local-goose-dialogue-thread.db`
- `local-goose-dialogue-completion.db`
- `local-goose-dialogue-responder.db`
- `local-goose-dialogue-responder-retry.db`

The helper requires an actual directory and regular non-symlink files.
The retry audit DB is opened using
`LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only`,
which requires an existing owner-owned `0600` DELETE-journal snapshot,
rejects SQLite companion files and future/unknown schema, and never creates
an absent retry file.

The historical store and TaskStore constructors are **not** claimed to be
read-only filesystem openers: they may initialize SQLite state. They must
therefore only see **separately copied, disposable** rehearsal databases,
never live production paths or read-only host mounts. This distinction is
mandatory even though the eligibility service itself holds their handles
by const reference.

The helper calls `inspect_generation_1` for the exact immutable
generation-1 lineage, held human sequence 4, cursor 3, preferred head
revision 3 and terminal historical authority evidence. A successful result
prints `ELIGIBILITY=evidence_consistent_no_authority` and
`GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_INSPECT=PASS`.

A FAIL prints `...=ABORTED` and exits nonzero. No automatic retry,
write, recovery, authority preparation, dispatch, Task or ACK exists.

## Image and CI boundary

`Containerfile.responder-retry-frozen-snapshot` creates a separate
Fedora 44 helper image with only this entrypoint and the pinned core.
It is never the production `Containerfile`, Quadlet image, `main.cpp`
or LiveControl surface.

The dedicated CI rejects mutable service calls and runtime wiring;
compiles the standalone helper on Fedora 44 with `-Werror`;
tests `--help`, unsupported operations and missing-snapshot fail-closed
behavior. This is **not a Fedora production proof**.

## Required next separate gate

Before any real Fedora snapshot inspection:

1. Design and merge a dedicated versioned **isolated proof runner** with
   an exact authorization token and clean pinned checkout.
2. Check actual production image `93811d1e201641ccfe0a31bc5ebb53458e7794fd2f727f18ff91b7ff246ba071`,
   `Network=none`, provider total 10, authority dormant,
   retry lineage present in production, held sequence 4 and head 3.
3. Stop the service briefly only for a stopped-state backup and consistent
   SQLite copies of the four historical databases **plus** the existing
   retry sidecar; restart the unchanged service before inspection.
4. Give the helper **only** the private `/rehearsal` mount. Verify all
   five logical/raw source evidence hashes around inspection. A success
   must leave production bytes and state unchanged; the copied historical
   stores may perform their ordinary opening behavior, which is why
   source hashes and the production state must be checked separately.
5. Reverify provider, network, production image, responder cursor 3,
   sequence 4 pending/non-ACKed, head 3 and immutable audit lineage.
   On uncertainty, fail closed without a recovery dispatch or automatic
   production restoration.

This future Fedora operation requires its own explicit human gate and
real PASS. No retry grant, lease, decision, intent, disposition or Task
follows from an eligibility PASS. Never alter or reuse the historical
sequence-4 authority schema-v3 rows. Provider/OpenAI stays OFF.
