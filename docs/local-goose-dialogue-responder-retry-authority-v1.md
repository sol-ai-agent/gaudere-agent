# Local Goose dialogue responder retry authority v1

## Status

This document defines the first dormant storage tranche for explicitly retrying a responder intervention after a previous decision-authority attempt became unusable before dispatch.

It does **not** authorize a retry, create a grant or lease, prepare an intent, submit a Task, advance a thread head, or ACK a completion event.

## Why a separate retry lineage exists

The schema-v3 responder authority ledger intentionally models one durable decision and one durable disposition for one consumer/event. Its v1 disposition identity is derived from the exact completion evidence, consumer identity and disposition kind. Repeating the same intervention would therefore collide both with schema-v3 uniqueness and with the canonical v1 disposition id.

The historical attempt must remain immutable. Reusing, reopening, renewing or rewriting its grant, lease, decision, intent or disposition would erase the distinction between the authority that expired and any later authority explicitly granted for a retry.

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
- retry generation exactly `1`;
- the root historical grant, lease, decision, intent and disposition ids;
- the historical lease/decision/intent/disposition terminal reasons;
- the historical grant terminal reason when one exists; an empty value is canonical when the grant remains activated but its authority window is already expired and irrevocably bound to the historical lease;
- a creation timestamp, which is audit metadata and is intentionally excluded from identity.

The lineage id hashes every immutable field above except the creation timestamp. Replaying the same explicit lineage request at a later time therefore resolves to the same id and cannot create a second logical retry.

## Exactly one retry in v1

Retry authority v1 permits exactly one generation-1 lineage for a consumer/event and exactly one for the corresponding consumer/completion sequence.

The SQLite ledger enforces:

- unique `request_id`;
- unique `(consumer_id,event_id)`;
- unique `(consumer_id,completion_sequence)`;
- `retry_generation = 1`.

There is no predecessor field and no generation-2 path in this version. If the generation-1 retry also becomes unusable, another retry requires a new versioned contract and a new explicit gate. Nothing in this v1 store can silently create or authorize a second retry.

## Sidecar boundary

The dedicated retry sidecar owns schema v1 and one table:

`local_goose_dialogue_responder_retry_lineage`

The sidecar is a current-user regular file mode `0600`, rejects unknown future schemas, and uses an append-only API. There is no update or delete operation in the store interface.

It is deliberately separate from the production schema-v3 responder sidecar. This tranche never opens or mutates the historical authority database.

## What this tranche cannot prove

The retry-lineage store validates canonical identifiers, evidence shape and the exactly-one-retry boundary. It does not have read authority over the historical responder sidecar, completion feed, thread store or Task store.

A later typed retry-authority service must re-read those authoritative stores and prove, at minimum, that:

1. the completion event is still the exact next unACKed event for the consumer;
2. the preferred thread head is still the exact historical human head;
3. the referenced historical decision, intent and disposition are terminal with the exact recorded reasons;
4. the historical lease is expired with the exact recorded reason and committed zero turns for that failed attempt;
5. the historical grant is either terminal with the recorded reason, or remains activated only for the historical lease and its grant authority window is already expired;
6. no durable submission Task exists for the terminal historical intent;
7. a **new** explicit grant and a **new** lease admit the retry;
8. no generation-1 retry lineage already exists for the exact event or completion sequence.

Only after those proofs may a later tranche admit retry-specific decision/disposition authority and prepare a new intent.

## Production boundary

This first tranche is store-only and deliberately absent from:

- `src/main.cpp`;
- the production `Containerfile`;
- `src/Makefile.am` runtime targets;
- the production service wiring.

It has no Runtime, TaskStore, completion-feed ACK, provider/OpenAI, scheduler, stimulus, shell, systemd or Podman capability.

The provider remains off and no production authority state changes merely because this code exists or is merged.

## Sequence 4 recovery application

The production sequence-4 historical decision, intent, disposition and lease have been recovered to terminal states without a submission or ACK. The associated historical grant was not rewritten by that recovery; it remains immutable and tied to the expired historical lease.

This design therefore permits one future generation-1 lineage to represent that exact recovered chain without inventing a grant terminal transition that did not occur and without modifying any historical row.

Creating that real lineage is a later explicit gate. This tranche itself creates no production lineage and does not ACK sequence 4.
