# Local Goose V1 → V2: read-only dormant bridge preflight

Status: **source-only proof, never an activation gate**. The implementation
`LocalGooseV2BridgeProof` does not open a database, write a cursor, create
or submit a Task, consume a stimulus, call Goose/OpenAI, configure B10,
schedule a wake, or modify Fedora.

## Authority boundary

Existing V1 cursor scope is fixed to `cognition.local-goose-cycle.v1`.
The observed generation=2/revision=3/dormant on Fedora identifies the
**next opportunity**, not a mandate to execute generation 2. A dormant V1
decision with null wake delay cannot be reinterpreted as `continue_local`
to bootstrap V2. The historical local-activity pulse is a separate lineage.

This pure preflight receives two trusted callbacks from the sole Agent owner
thread: an exact fixed-scope V1 cursor read and an exact-ID bounded Task read.
The pinned model SHA comes from trusted Agent configuration. It performs:

1. One structurally validated V1 cursor read; require `dormant` with an
   existing predecessor (generation ≥ 2). Any scheduled, prepared, blocked or
   malformed cursor is inapplicable or rejected.
2. Verify exact predecessor Task identity, canonical succeeded result bytes,
   SHA-256, generation, anchor and pinned model; verify dormant no-wake
   decision (idle or request_openai, delay null). Neither enum grants authority.
3. Verify immutable generation-3 observation anchor and its exact result digest.
4. Reconstruct bounded four-entry V1 memory using existing pure builder:
   at most three predecessor-link reads, no scan and no skipping recent Tasks.
5. Read **the entire cursor a second time**, checking all fields, not just
   revision. Report only a digest of the bounded memory, predecessor
   fingerprint, revision, generation and omission count; no private prose.

The total budget is **two cursor reads and five exact Task reads**. The
success status deliberately reads `verified_dormant_no_authority`; it
proves an internally consistent *observation*, never readiness to schedule
or submit V2. After return, the cursor may change. No read-only snapshot
can substitute for an atomic durable single-writer transition.

## Crash/replay, failure and rollback invariants

Re-reading the same unchanged durable snapshot produces the same bounded
memory digest and predecessor identity, including after a process restart.
A missing predecessor, changed model, unexpected Task result, truncated
lineage, missing anchor, malformed JSON or changed cursor never falls back
to empty memory or an unlinked V2 root. Exception details and raw stored
model/provider text are not returned.

**Still unimplemented and separately gated**:

- Durable, versioned V1↔V2 migration *record*, with explicit human-authorized
  decision, sole-writer lease and atomic fencing of all competing V1 writes.
- A frozen V2 input bound to exactly that durable migration record and one
  generation; restart must reuse the *same* previously committed Task ID.
  A preview rederived after a crash is not a safe idempotent execution plan
  until this is proven transactionally.
- Compensation/rollback before and after first V2 commit, protecting both
  historical V1 evidence and V2 lineage, with proof no double-submission,
  no conflicting cursor, provider OFF and no implicit dormant wake.
- Separate source tests and isolated Fedora proof with a **new** explicit
  operator authorization for any host mutation or real V2 Task.

Do not use this diagnostic to decide on Fedora release readiness or to
claim that the dormant cycle ought to wake. Source tests are fake-store
checks only. No portal/telemetry/statistics ingestion or B10 integration.
