# WakeIntent v0 recovery and arming observability design

Status: **PREP ONLY / DESIGN ONLY / NO PRODUCTION AUTHORIZATION**

Work item: [gaudere-agent#60](https://github.com/sol-ai-agent/gaudere-agent/issues/60)

Agent baseline: `49a026228e811b808ba4252e09da49fb6f97820b`

Pinned Core: `c24c40b84a12e51515cee4611e3dc79e9fd83892`

Related evidence: [PR #58](https://github.com/sol-ai-agent/gaudere-agent/pull/58)
defines W26/W27; [PR #62](https://github.com/sol-ai-agent/gaudere-agent/pull/62)
contains the separate test-only P1 tranche for issue #59.

This document does not add an API, change a runtime, enable `--wake-intents`, open a
port, inspect the production database, accept a WakeIntent, or call a provider.
Production remains schema v3 with WakeIntent disabled.

## Decision

W26/W27 should be closed with one bounded read-only Core inspection primitive and
one composite Agent live-control report:

1. Core inspects at most two records for the runtime's already-fixed scope and
   returns `empty`, `one`, or `ambiguous`;
2. Agent resolves the record's source Task through the existing owner-held
   `TaskStore`;
3. the main worker compares the durable wake deadline, the next Task lease
   deadline, and `Scheduler::next()`;
4. `gaudere-control wake-status` reports the durable record and a conservative
   deadline-coverage classification without requiring a wake ID;
5. inconsistent or ambiguous state fails the command closed and remains a human
   recovery condition; inspection never repairs or mutates it.

No durable `armed` flag should be added. `scheduled` remains the durable truth;
arming remains derived, in memory, and recoverable from SQLite after restart.

## Current gap

The current surfaces are individually safe but cannot answer the recovery question
when the operator has lost the wake/source identity:

| Existing surface | What it proves | Missing evidence |
|---|---|---|
| `WakeIntentStore::find(scope, id)` | exact durable record by known wake ID | requires the lost ID |
| `find_by_source(scope, source_id)` | idempotent source lookup | requires the lost source Task ID |
| `next_scheduled_at(scope)` | earliest scheduled deadline | omits identity and all terminal records |
| `gaudere-control wake WAKE_ID` | full durable report for one known record | cannot discover the fixed-scope record |
| `Scheduler::next()` | actual in-memory next deadline | does not identify whether wake, lease, or an earlier notification selected it |
| `Runtime::next_recovery_at()` | exact next Task lease deadline | does not expose wake state |

Opening `state.db` from another process while the service owns it is not an
acceptable workaround. Requiring an external note containing the ID is useful
operator hygiene, but it is not recovery observability.

## Requirements

The read surface must:

- discover zero or one Agent-v0 record for
  `cognition.reflect.wake.v0` without caller-supplied identity;
- fail closed rather than select arbitrarily if more than one record exists;
- expose durable `status`, `accepted_at`, `due_at`, source identity, terminal time,
  and terminal reason;
- resolve enough source Task metadata to establish that the durable relationship
  still exists without duplicating user-visible provider output;
- distinguish exact wake coverage, an equal shared lease deadline, safe coverage by
  an earlier event, and missing/late scheduling;
- execute only in the existing main worker through the existing AF_UNIX mailbox;
- perform no write, provider action, permit consumption, network request, successor
  creation, periodic wake, or automatic recovery;
- remain bounded in input, rows, output, and runtime;
- give automation a stable exit status while retaining a human-readable report.

The first implementation does not need causal attribution inside `Scheduler`.
Comparing its actual deadline with the two durable derived deadlines is sufficient
to prove whether the scheduled wake is covered. Stronger provenance would be a
separate tranche.

## Options considered

| Option | Cost | Recovery quality | Verdict |
|---|---:|---|---|
| Require humans to retain `WAKE_ID` in continuity | no code | loses W26 when the record is forgotten or continuity is unavailable | fallback only |
| Query SQLite directly from the helper or a sidecar | small local script | duplicates schema validation and violates the live-owner boundary if used while running | reject for live use |
| Add a second durable registry or durable `armed` bit | new writer/schema/state reconciliation | creates two truths and crash windows for state that is deliberately derived | reject |
| Add an unbounded Core `list_all` API | moderate | discovers records but unnecessarily exposes arbitrary scopes/cardinality | reject |
| Add bounded fixed-scope inspection plus an Agent composite report | small read-only Core/Agent APIs | discovers identity and proves deadline coverage without a second writer | **recommend** |
| Add a WorkController provenance snapshot | extra in-memory state/API | can name the last selector precisely, but is not required to determine safe coverage | defer unless review proves necessary |

## Recommended Core contract

The generic Core store needs a bounded ambiguity-aware query. A representative
contract is:

```cpp
enum class WakeIntentScopeResult {
    empty,
    one,
    ambiguous
};

struct WakeIntentScopeInspection {
    WakeIntentScopeResult result = WakeIntentScopeResult::empty;
    std::optional<WakeIntent> intent;
};

class WakeIntentStore {
public:
    [[nodiscard]] virtual WakeIntentScopeInspection inspect_scope(
        const std::string& scope) const = 0;
};
```

The SQLite implementation executes one ordered query with `LIMIT 2`. It validates
every returned row through the existing `read_intent` path:

- zero rows → `empty`, no intent;
- one valid row → `one`, that intent;
- two rows → `ambiguous`, no arbitrarily selected intent;
- malformed persisted data → exception/fail-closed, as today.

The limit is deliberately two: Agent v0 needs only to distinguish `0`, `1`, and
`more than 1`. It does not need total counts or an unbounded list. The query is
read-only, creates no table/index, and requires no schema-version change.

`WakeIntentRuntime::inspect_scope()` should delegate using its constructor-fixed
scope; callers must not be able to supply another scope. This preserves the current
application-selected namespace boundary.

## Recommended Agent contract

`ExplicitWake` should compose the Core inspection with its existing fixed policy
and `TaskStore`:

```text
scope inspection
    → exactly one durable WakeIntent or empty/ambiguous
    → TaskStore.find(intent.source_id)
    → source consistency classification
```

The source summary should include only:

- Task ID;
- kind;
- status;
- attempts;
- result content type;
- consistency: `eligible`, `missing`, or `ineligible`.

The existing `task SOURCE_TASK_ID` command remains the explicit path for the full
Task result. `wake-status` should not repeat model text or provider metadata merely
to prove the relationship.

Source eligibility should reuse the same canonical-decision validator as
acceptance. It must not create a second, weaker interpretation of
`propose_wake`.

### Scheduler observation

`LiveControlProcessor` already executes on the sole worker and already owns the
work `Runtime`. Give it one narrow observational callback for `Scheduler::next()`;
do not give the socket thread Runtime, store, scheduler, or status access.

The worker can then read, in one bounded command:

1. fixed-scope durable record and `WakeIntentRuntime::next_scheduled_at()`;
2. `Runtime::next_recovery_at()` for Task leases;
3. `Scheduler::next()` for the actual in-memory deadline.

No `WorkController` behavior change is required. If a control notification is the
earliest current deadline, the result is truthfully
`covered_by_earlier_event`; the following worker event recomputes durable deadlines
as it already does.

## Live-control protocol

Add a provider-free command with no caller-selected identity:

```sh
sh scripts/control-service.sh wake-status
```

The existing protocol envelope currently requires an `id`. The client can use the
fixed internal sentinel `current`; the parser must accept only that sentinel and no
text for the new operation. The user-facing command remains argument-free.

The socket remains `/tmp/gaudere-control.sock`, mode `0600`, inside the container.
No host socket, TCP listener, HTTP endpoint, or direct database reader is added.

### Stable report

A v1 line report should expose at least:

```text
report_schema="gaudere.wake_status.v1"
scope="cognition.reflect.wake.v0"
record=one
health=ok
id="SOURCE_TASK_ID"
source_task_id="SOURCE_TASK_ID"
source_task_kind="cognition.reflect.v1"
source_task_status=succeeded
source_consistency=eligible
status=scheduled
accepted_at_ms=...
due_at_ms=...
terminal_at_ms=none
terminal_reason=""
derived_wake_at_ms=...
derived_lease_at_ms=none
scheduler_next_at_ms=...
scheduler_coverage=exact
```

String values must use the existing JSON-safe quoting. Timestamps are signed
decimal epoch milliseconds or `none`; raw JSON is not embedded.

For `record=none`, identity/status fields are omitted and
`scheduler_coverage=not_applicable`. For `record=ambiguous`, no record is selected.

### Deadline classification

Let `W` be the persisted `due_at` for the one `scheduled` record, `D` the value of
`next_scheduled_at()` for the fixed scope, `L` the next Task lease deadline, and
`S` `Scheduler::next()`.

| Durable/derived relation | Classification | Command result |
|---|---|---|
| no record | `not_applicable` | success, `health=empty` |
| terminal `fired` or `revoked` | `not_applicable` | success, `health=terminal` |
| `manual_review` | `not_applicable` | fail closed, human review required |
| scheduled and `D != W` | `derived_mismatch` | fail closed |
| scheduled and no `S` | `missing` | fail closed |
| scheduled and `S > W` | `late` | fail closed |
| scheduled and `S == W`, `L != W` | `exact` | success |
| scheduled and `S == W == L` | `shared_with_lease` | success |
| scheduled and `S < W` | `covered_by_earlier_event` | success |
| more than one fixed-scope record | `ambiguous` | fail closed |
| missing/ineligible source Task | `not_applicable` | fail closed |

An earlier scheduler event is safe coverage, not a false alarm: the sole worker
will reconcile and then reselect the exact minimum. A later or missing scheduler
deadline is not repaired by inspection.

Normal observations return code `0`. Ambiguity, source inconsistency,
`manual_review`, `derived_mismatch`, `missing`, or `late` return the existing
policy/error code `4` while still printing the bounded diagnostic body. Transport
or internal exceptions remain code `1`; malformed commands remain code `2`; an
explicit known-ID lookup can continue to use code `3` for not found.

## Restart and crash semantics

The status API must describe existing behavior rather than introduce recovery
behavior.

### Normal restart with a future wake

1. stores open under the existing single-process lock;
2. `WorkController::start()` reconciles durable wake state;
3. it schedules an immediate startup event and the exact minimum durable deadline;
4. live control starts only after these mandatory resources exist;
5. `wake-status` returns the same immutable identity/times and either `exact`,
   `shared_with_lease`, or safe earlier-event coverage.

### Restart at or after due

Startup reconciliation records one `fired` transition before service readiness.
Status then returns terminal state and no derived wake deadline. It creates no Task
or provider work.

### Clock before `accepted_at`

Startup reconciliation durably enters `manual_review(clock_rollback)`. Status is
read-only and returns a nonzero policy result. It never resumes, deletes, refunds,
or rearms the record.

### Crash after acceptance commit but before reply/refresh

The one committed `scheduled` row remains authoritative. Restart discovers it by
scope and re-arms the immutable deadline. Retrying acceptance with the original
source remains duplicate/idempotent. This is the W07 path exercised separately by
PR #62.

### Durable/arming divergence

If a future scheduled row reports `missing`, `late`, `derived_mismatch`, ambiguous
scope state, or an inconsistent source, the command must not:

- call `refresh_deadlines()` as a hidden repair;
- rewrite the wake to `manual_review` merely because it was inspected;
- revoke it automatically;
- delete/refund the lifetime slot;
- retry a provider or create a successor.

It reports the evidence and leaves the human recovery path authoritative.

## Human recovery protocol

This is a contract for a future implementation, not authorization to run it on the
current production service.

1. Run one live `wake-status` through the owner process; never open `state.db`
   concurrently.
2. If health is `ok`, `empty`, or expected `terminal`, retain the report with the
   exact Agent/Core/image IDs and continue only within the separately authorized
   gate.
3. If health is fail-closed, stop the user service and prove it is inactive.
4. Take the existing flock-protected backup before any offline diagnostic action.
5. Inspect only a disposable restored copy first. Preserve the original database,
   WAL/SHM evidence, service profile, and image identities.
6. A single controlled restart may prove deterministic re-arm/reconciliation. Run
   one status command after readiness.
7. If divergence persists, keep the service stopped and escalate to Sol/Bertrand
   for explicit restore, rollback, or forensic review. Do not delete or recreate
   the wake and do not refund its lifetime slot.

There is no periodic monitor, automatic restart loop, blind retry, or automatic
provider action in this protocol.

## Invariants retained

- exactly one process owns the state database through `state.db.lock`;
- only the main worker reads Runtime/store state for the composite report and only
  that worker performs mutations elsewhere;
- the signal/control threads retain atomics, mailbox, and wake-only authority;
- Core inspection is fixed-scope and `LIMIT 2` bounded;
- live-control input and output retain existing byte bounds;
- `scheduled` is the only durable arming truth; no second durable bit exists;
- scheduler, lease, and wake deadlines remain exact one-shot events;
- there is no polling, TCP port, sidecar, shell command, provider, secret, or
  permit consumption;
- terminal state and lifetime use remain irreversible by automatic code;
- human backup, stopped-service inspection, restore, and rollback remain possible.

## Proposed tests

### Core read-only API

- invalid scope is rejected;
- zero rows returns `empty`;
- one scheduled, fired, revoked, or manual-review row round-trips exactly;
- two generic Core records in one scope return `ambiguous` and no selected record;
- the query never returns more than two rows and never mutates schema/data;
- malformed persisted records fail through existing validation;
- mock stores and all construction orders implement the new pure virtual method.

### Agent composition and report

- disabled capability rejects `wake-status` without opening wake state;
- no-ID discovery returns empty/one/ambiguous correctly;
- source Task eligible, missing, wrong kind/status/result, and noncanonical decision
  produce the specified consistency result;
- report quoting and all timestamp/status variants are stable;
- exact, shared lease, earlier event, missing, later, derived mismatch, terminal,
  and ambiguous deadline classifications are deterministic;
- the argument-free CLI and helper use only the fixed sentinel internally;
- status does not call acceptance, revocation, reconciliation, refresh, provider,
  Task submission, or Action APIs.

### Crash/restart integration

- W07 hard-exit fixture restarts, discovers by scope without an ID, and reports the
  original exact `due_at`;
- future restart, overdue restart, clock rollback, pre-due revoke, and fired state
  produce the expected report and exit code;
- lease earlier than wake and equal lease/wake deadlines report safe coverage;
- snapshots prove Tasks, Actions, provider metadata, budget rows, and wake state are
  byte-equivalent before/after repeated inspections;
- idle service after inspection has no periodic deadline;
- current offline/OpenAI installers still omit `--wake-intents` and no production
  validator calls `wake-status` before a separate activation authorization.

## Implementation split requiring Sol approval

No implementation is authorized by issue #60. If Sol accepts this design, split it
into reviewable, still-PREP-only work:

1. **Core read-only PR:** bounded scope inspection, runtime delegation, mock/SQLite
   tests; no schema change.
2. **Agent read-only PR:** composite `wake-status`, deadline classification,
   client/helper/docs/tests, then pin the reviewed Core commit; no profile change.
3. **Optional provenance PR only if required:** worker-owned in-memory snapshot of
   which derived deadline last selected the scheduler. Do not add it merely for
   nicer wording; raw deadline coverage is already sufficient for safety.
4. **Future gate integration:** only after production is separately migrated to v4,
   the wake-off service gate has passed, and `wake-prod-01` is explicitly authorized.

The first two PRs must remain usable only when the already-existing capability is
explicitly enabled in a disposable environment. They do not authorize installing
that flag or creating a wake.

## Acceptance criteria for the future read-only tranche

- an operator can recover the only fixed-scope identity with `wake-status` and no
  prior ID;
- ambiguity and source inconsistency fail closed;
- future scheduled state reports exact/shared/earlier coverage or a nonzero
  divergence result;
- restart preserves identity and deadline and requires no periodic worker;
- repeated inspection is byte-for-byte state inert;
- all historical CI and service-config checks remain green;
- deployed profiles remain unchanged and production remains untouched.

This is the smallest design that closes W26/W27 without weakening Gaudere's
single-owner, event-driven, reversible architecture.
