# Responder retry generation-1 — post-admission authority boundary

Status: **design and proof contract only**. This document creates no grant, lease, decision, intent, disposition, Task or ACK; it does not authorize a Fedora gate or provider use.

## Evidence and prerequisite

As checked on 2026-11-03, `sol-ai-agent/gaudere-agent` main was `3d781b3898b1f91caa165df7277fb0d3fcbad6d1`, after the squash merge of #269. The #268 real Fedora proof was reported PASS on a **copied** snapshot, with exact replay yielding `duplicate`, four production SQLite logical hashes unchanged, and no retry sidecar in production.

The #269 production-admission runner is merged and CI-green, but **there is no recorded Fedora production PASS yet** in the checkpoint considered here. This design depends on that independently authorized gate. A merge is not a production state transition.

The only recognized generation-1 lineage in the isolated proof was:

`cognition.local-goose-dialogue.responder-retry-lineage.v1:da6bb42704949f0003e670c06e7ed04078c238f08356f1853f9b3776455f2bca`

Its production existence must never be inferred from code, CI, the rehearsal artifact, or this document.

## Existing architectural collision

The legacy responder authority schema-v3 table `local_goose_dialogue_responder_decision` has a `UNIQUE(consumer_id,event_id)` constraint. The historical sequence-4 event already has a terminal decision. The legacy responder disposition v1 is likewise one disposition per event, and its identity excludes retry generation. The historical decision, intent, disposition, grant and lease must remain attributable and immutable.

Therefore **a retry cannot be modeled by simply invoking the ordinary v1 `prepare_decision` / `prepare_disposition` for the same event**. Nor may it reset historical rows or recycle the expired grant/lease, adjust `consumer_id` to bypass uniqueness, or route a Task directly around the dispatcher.

The lineage sidecar schema-v1 stores **audit admission, not execution authority**. Its uniqueness `(consumer_id,event_id)`, `(consumer_id,completion_sequence)` and `retry_generation=1` intentionally prevents a second lineage.

## First code tranche after #269: read-only eligibility

Implement a typed, provider-free `RetryAuthorityEligibility` / `inspect`-like service **without mutations**. Its output must distinguish:
- no production lineage / lineage present with exact canonical ID;
- recovery state and historical authority evidence consistent/inconsistent;
- current human event/head still held and exact / stale or ACKed;
- no submission Task / Task already present / ambiguous;
- authority absent (normal) / independently prepared later.

Inputs must be bound to the same snapshots and authenticated evidence already used by the admission service: fixed responder consumer `system-responder-v1`, sequence 4 event/result/head/task, generation 1 lineage, historical grant/lease/decision/intent/disposition and terminal causes, completion cursor, and TaskStore.

**Do not automatically turn an `eligible` result into a grant, decision, intent or dispatch.** On missing, stale or internally conflicting evidence, return a non-mutating fail-closed result.

Read-only inspection must not initialize an absent retry sidecar or normalize legacy rows. Its C++ service references to historical evidence stores should be `const` as in the existing admission service.

## Separate authority design: questions that must be closed before implementation

1. **Namespace and uniqueness**: define retry-specific canonical identities that include the exact immutable `lineage_id` and generation 1. Design a separately versioned retry authority/decision/disposition persistence surface, or another explicitly proven equivalent that cannot collide with schema-v3 `UNIQUE(consumer_id,event_id)` and legacy disposition identity.
2. **Lease ownership and accounting**: decide where a new finite grant and lease live. The current bounded dispatcher and lease accounting operate on the legacy responder store. A separate retry store cannot silently be treated as a compatible lease source. Provide a typed and tested bridge before any retry dispatch, or explicitly add a new dispatcher/accounting contract; **never directly submit a Task to work around this**.
3. **Causality**: each new retry authority record must bind a single admitted lineage plus exact completion/head evidence and distinct new authority-source/request IDs; no reuse, reopening, renewal or rewrite of 6A/6B1/6B2 objects.
4. **Crash boundaries**: specify idempotent prepare / explicit activation / decision / intent binding / dispatch transitions and audit linkage. An uncertain external effect must enter durable manual review; no blind second attempt and no implicit ACK.
5. **One-attempt limit**: generation 1 is the entire v1 retry budget. A subsequent retry requires a new version and an independent human gate, even after a timeout or failed dispatch.
6. **Default isolation**: no observer coupling, `main.cpp` wiring, `Containerfile` promotion or production migration in storage/service code tranches. No provider/OpenAI, network, autonomous cycle/stimuli/wake, shell execution or auto-generated intervention text.

Do not grant a lease merely because an admissible lineage exists. The exact intervention message, finite TTL, speaker envelope and human authority must be supplied and validated independently.

## Minimum adversarial proof matrix

Before any state-writing authority tranche, cover at least:

- production retry lineage absent, duplicate/foreign/malformed lineage, schema too new;
- wrong generation or mismatched canonical lineage ID; mutated historical identifiers or terminal reasons;
- completion sequence 4 ACKed, cursor changed, preferred head changed, result hash changed, speaker no longer human;
- historical lease not expired, turns already committed, grant expired/activated evidence inconsistent;
- prior submission Task, independent Task contention, conflicting decision/disposition;
- legacy schema-v3 uniqueness collision on ordinary v1 decision; prove retry path cannot mutate or replace the old row;
- replay of a prepared grant or decision is exact-idempotent; conflicting request key fails closed;
- crash/restart between each stage does not synthesize a missing grant, lease, decision, intent, Task or ACK;
- downstream dispatch cannot be reached from audit admission or read-only eligibility;
- production image, provider count 10, `Network=none` and human sequence4 held through all isolated proofs.

All tests first run on fabricated fixtures, then on a separately authorized Fedora snapshot of the *actual* post-#269 state. Preserve the four historical database logical hashes whenever a tranche purports to be audit-only/read-only.

## Safe ordering and explicit gates

1. **#269**: verify exact CI, merge and independent Fedora PASS for adding one production retry lineage; stop if absent or ambiguous.
2. **Read-only eligibility**: code + adversarial CI + merge; optionally isolated Fedora proof on copied state with a separate gate if necessary. No production mutation.
3. **Retry-specific authority schema and store**: design/implementation + CI, without production migration or runtime wiring.
4. **Typed retry grant/lease service**, disabled by default, with separate prepare and activation. This must not create a decision or intent.
5. **Retry decision and intent/disposition binding**, with retry-aware uniqueness and exact lineage evidence. No Task or ACK.
6. **Retry-aware dispatch/accounting proof**, isolated before any production code promotion.
7. **Production code deployment / migration / grant / decision / dispatch**: distinct, explicitly approved steps with their own Fedora real proofs and rollback/fail-closed contracts.

At every stage: `code -> CI success -> merge -> (separately authorized Fedora operation) -> real Fedora proof`.

The current production defaults remain provider/OpenAI OFF, `Network=none`, authority disabled, sequence 4 non-ACKed and no autonomous execution. This document never constitutes authorization for any transition.
