# Autonomous current-cognition pulse v0 — persistence decision

Status: **normative addendum** to `autonomous-current-cognition-pulse-v0.md`.

The generic Gaudere Core SQLite state is currently schema v4 and its tables model generic work, Action, budget and WakeIntent concepts. The autonomous cognition cursor is instead an Agent-specific authority state. The v0 implementation therefore **must not add a pulse table to the Core database and must not introduce a Core schema migration**.

## Agent-owned sidecar

The pulse cursor lives in a dedicated SQLite sidecar owned by Gaudere Agent, under the same Gaudere state directory, with an explicit path supplied to the isolated implementation/proof binary. A future production default may be:

```text
~/.local/share/gaudere/autonomy/pulse.db
```

The exact production path is frozen only by a later activation gate.

Sidecar schema starts at version 1 and contains only the fixed-scope pulse cursor defined by the main design. It uses the same conservative SQLite durability settings as the existing stores (`WAL`, `synchronous=FULL`, bounded busy timeout) but has no Provider, Action, budget or WakeIntent tables.

## Why split storage is acceptable

The pulse cursor and the generic Task database cannot commit atomically across two SQLite files. The design therefore relies on **ordered, deterministic recovery**, not cross-database transactions:

1. freeze `observed_at_ms` durably in the sidecar first;
2. derive the context request solely from the frozen cursor plus read-only canonical Gaudere state;
3. record the content-addressed snapshot using the frozen observation time;
4. claim the existing deterministic `cognition.current.v0` Task;
5. only after exact validation, persist the snapshot/current Task ids in the sidecar and mark it `prepared`.

Crash cases are deliberately recoverable:

- crash after step 1: regenerate exactly the same context request and snapshot identity;
- crash after snapshot persistence: recorder returns the exact validated duplicate;
- crash after current Task persistence but before cursor update: `CurrentCognitionCycle` returns the exact validated duplicate;
- crash after cognition becomes terminal but before pulse settlement: settlement re-reads that exact named Task and advances the cursor once.

No recovery path is allowed to replace `observed_at_ms`, select a newer predecessor, create a second cognition identity or replay a provider effect.

## Locking and ownership

The first implementation remains an isolated provider-free proof and is not wired into the production service. It must take an explicit sidecar lock/SQLite write transaction before cursor mutation and use the existing Gaudere state lock rules when it needs consistent mutation of generic Task state.

A later persistent-service integration must establish one process as the sole writer of both Agent sidecar and generic Gaudere state. B10/manual one-shots must not race that owner.

## Backup and rollback implication

Once the sidecar is ever activated in production, stopped-state checkpoints and rollback runbooks must back up and restore **both**:

- the generic Gaudere state database;
- the Agent autonomy sidecar.

Until production activation, no sidecar exists in production and existing rollback evidence remains unchanged.

## Proof additions

The provider-free implementation proof must additionally demonstrate:

1. opening/initializing the sidecar does not alter Core schema v4;
2. generic DB table set remains unchanged;
3. crash boundaries across the two databases converge to exactly one snapshot/current Task identity;
4. deleting a synthetic sidecar and reseeding in a test fixture does not mutate historical Task/Action/budget/Wake evidence;
5. a corrupt/unsupported sidecar schema fails closed rather than being recreated silently.

This choice keeps the recurring autonomy mechanism inside Gaudere Agent, leaves the generic Core schema unchanged, and makes the later production activation/rollback surface explicit.