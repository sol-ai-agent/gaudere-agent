# Retry generation 1 — isolated scope ledger, not execution authority

Status: **source and CI only**. This tranche follows #270's collision contract and the
operator-reported Fedora #274 frozen-copy proof on 2026-11-07:
`FEDORA_GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_ISOLATED=PASS`.
The proof reported `ELIGIBILITY=evidence_consistent_no_authority`, five production
SQLite hashes unchanged, retry audit bytes unchanged, sequence 4 unACKed and no
submission Task. The repository's production image and service were not changed
by this code tranche.

## Boundary

The historical responder sidecar schema-v3 already has one terminal decision
per `(consumer_id,event_id)`, and the historical disposition identity also
excludes retry generation. No second attempt may reopen or overwrite that
original chain.

This tranche introduces **an entirely separate, versioned SQLite file format**
`LocalGooseDialogueResponderRetryScopeStore`. The file has no prescribed
production path and is not installed or constructed by any production runtime.
It contains only an immutable, canonical **evidence scope** for one candidate
generation-1 retry. `stage_scope` means *record an isolated proposal without
authority*. The record does **not** contain a human grant, lease, message,
permission, decision, intent, disposition, dispatcher state or Task.

A scope records the admitted lineage ID, independent request key, original
consumer/event/result/Task/head/speaker, generation 1, sequence 4 and a
versioned SHA-256 scope identity. The maker requires a canonically validated
lineage object, but the store cannot independently prove that a lineage is
physically present in the actual production audit DB or that live eligibility
still holds. A later typed, separately gated service must reopen and validate
those authoritative facts. **Never infer permission from a staged scope.**

## SQLite invariants

- The new schema v1 is separate from both the historical responder SQLite and
  the audit-lineage SQLite. No schema migration or update of either is present.
- `UNIQUE(lineage_id)`, `UNIQUE(request_id)` and
  `UNIQUE(consumer_id,event_id,retry_generation)` enforce at most one scope
  for the exact generation-1 retry, without changing legacy uniqueness.
- Canonical records are immutable. Atomic `BEGIN IMMEDIATE` staging accepts
  one insertion, returns `duplicate` for exact replay, `conflict` for a changed
  request, lineage, event or payload; no replace, update or deletion path.
- Only explicit `create_new_isolated` may create a **previously absent** file
  via `O_EXCL | O_NOFOLLOW`, owner regular mode 0600.
  `existing_read_only` requires a preexisting file and uses SQLite
  `SQLITE_OPEN_READONLY`. `existing_writer` is for explicit, isolated replay.
- All open modes reject symlinks, unsafe owner/mode and any
  `-wal`, `-shm`, `-journal` companions. Open validates exact user schema
  version 1, DELETE journal and the sole expected table DDL; any unknown or
  future schema fails closed. No automatic upgrade or recovery is supplied.
- No runtime, observer, `main.cpp`, production Containerfile, Automake,
  gateway, Fedora host script, provider, TaskStore or completion ACK coupling.

## Test and authorization gates

The Fedora 44 standalone CI compiles under `-Werror` with SQLite and the
existing SHA-256/lineage canonicalizer. Synthetic tests cover one insertion,
exact replay after reopen, conflicting request/lineage/event, generation 2,
wrong sequence and speaker, read-only nonmutating access, missing path,
symlink, 0644 mode, companions, foreign DDL and future schema.

This CI is **not** a Fedora host proof, a deployed production sidecar or any
grant of operational authority. The original provider/OAI remains OFF;
production `Network=none`; sequence 4 must remain pending at cursor 3,
preferred head revision 3, with no new Task.

Future steps, **each independently reviewed and gated**:

1. Typed bounded grant/lease preparation and separate human activation,
   tied to a fresh explicit authority source and this exact lineage; no
   authority from mere stage/eligibility.
2. Retry-specific decision/intent/disposition with generation-aware
   uniqueness and immutable historical provenance; no legacy identity reuse.
3. A typed, tested retry-aware dispatcher/lease accounting bridge. Never
   bypass the dispatcher with a direct Task save.
4. Adversarial crash/restart and isolated-copy proofs, then separately
   authorized host deployment, migration, grant, activation and dispatch.
   Each transition requires its own genuine evidence. Uncertain effects
   enter durable manual review; no automatic retries or ACK.

Generation 1 is the entire v1 retry budget; another generation needs a new
version and explicit human gate.
