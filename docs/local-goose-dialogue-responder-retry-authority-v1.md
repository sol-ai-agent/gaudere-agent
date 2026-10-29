# Local Goose dialogue responder retry authority v1

## Status

This document defines the first dormant storage tranche for explicitly retrying a responder intervention after a previous decision-authority attempt became terminal before dispatch.

It does **not** authorize a retry, create a grant or lease, prepare an intent, submit a Task, advance a thread head, or ACK a completion event.

## Why a separate retry lineage exists

The schema-v3 responder authority ledger intentionally models one durable decision and one durable disposition for one consumer/event. Its v1 disposition identity is derived from the exact completion evidence, consumer identity and disposition kind. Repeating the same intervention would therefore collide both with schema-v3 uniqueness and with the canonical v1 disposition id.

A terminal historical attempt must remain immutable. Reusing, reopening, renewing or rewriting its grant, lease, decision, intent or disposition would erase the distinction between the authority that expired and any later authority explicitly granted for a retry.

The retry path is therefore a separate append-only sidecar and namespace. The existing responder/authority sidecar remains schema v3 and is not migrated by this tranche.

## Retry lineage object

Prefix:

`cognition.local-goose-dialogue.responder-retry-lineage.v1:`

One lineage binds:

- an explicit request id;
- responder consumer id;
- exact completion sequence and event id;
- exact result SHA-256;
- exact thread alias, revision and triggering V3 Task id;
- exact observed human speaker id;
- a retry generation in the bounded range 1..8;
- for generation > 1, the immediately preceding retry-lineage id;
- the root historical grant, lease, decision, intent and disposition ids;
- the terminal reason recorded for every member of that historical authority chain;
- a creation timestamp, which is audit metadata and is intentionally excluded from identity.

The lineage id hashes every immutable field above except the creation timestamp. Replaying the same explicit lineage request at a later time therefore resolves to the same id and cannot create a second logical retry generation.

## Linear generations

Generation 1 has no previous retry-lineage id.

Generation N > 1 must name an existing generation N-1 lineage with identical root event evidence and identical historical authority-chain evidence. The store rejects skipped generations and conflicting roots.

The SQLite ledger additionally enforces:

- unique `request_id`;
- unique `(consumer_id,event_id,retry_generation)`;
- unique `(consumer_id,completion_sequence,retry_generation)`;
- unique non-null `previous_retry_lineage_id`.

This prevents concurrent duplicate generations and branching successors.

## Sidecar boundary

The dedicated retry sidecar owns schema v1 and one table:

`local_goose_dialogue_responder_retry_lineage`

The sidecar is a current-user regular file mode `0600`, rejects unknown future schemas, enables SQLite foreign keys, and uses an append-only API. There is no update or delete operation in the store interface.

It is deliberately separate from the production schema-v3 responder sidecar. This tranche never opens or mutates the historical authority database.

## What this tranche cannot prove

The retry-lineage store validates canonical identifiers, evidence shape, bounded generations and lineage continuity. It does not have read authority over the historical responder sidecar, completion feed, thread store or Task store.

A later typed retry-authority service must re-read those authoritative stores and prove, at minimum, that:

1. the completion event is still the exact next unACKed event for the consumer;
2. the preferred thread head is still the exact historical human head;
3. the referenced historical grant/lease/decision/intent/disposition chain exists and is terminal with the exact recorded reasons;
4. no durable submission Task exists for the terminal historical intent;
5. the historical lease committed zero turns for that failed attempt;
6. a **new** explicit grant and a **new** lease admit the retry;
7. no previously admitted retry generation has already completed or has ambiguous durable submission evidence.

Only after those proofs may a later tranche create a retry decision/disposition and prepare a new intent.

## Production boundary

This first tranche is store-only and deliberately absent from:

- `src/main.cpp`;
- the production `Containerfile`;
- `src/Makefile.am` runtime targets;
- the production service wiring.

It has no Runtime, TaskStore, completion-feed ACK, provider/OpenAI, scheduler, stimulus, shell, systemd or Podman capability.

The provider remains off and no production authority state changes merely because this code exists or is merged.

## Sequence 4 recovery application

The production sequence-4 historical attempt has already been recovered to terminal state. This design permits a future generation-1 lineage to point to that exact terminal chain without modifying any of its rows.

Creating that real lineage is a later explicit gate. This tranche itself creates no production lineage and does not ACK sequence 4.
