# WakeIntent v0 threat model and adversarial proof matrix

Status: **PREP ONLY / SOURCE-AUDIT ONLY**  
Work item: [gaudere-agent#56](https://github.com/sol-ai-agent/gaudere-agent/issues/56)  
Audited Agent integration baseline: `2972ba796f0ef44d438e16efab4d89fb296d5574`  
Reconciled `main`: `5287ad0387425213b1873c3f64fea118bf3e32c7`, with the same
source tree (`673b1b8e192812b5a7c9de20be6be4c0a1585df3`) after a transient
one-shot branch-cleanup workflow was added and removed.  
Audited Core pin: `c24c40b84a12e51515cee4611e3dc79e9fd83892`

This document does not authorize a production schema migration, an installed
`--wake-intents` flag, a real WakeIntent acceptance, a provider call, or a
successor Task. Production remains schema v3 with the wake capability disabled.

## Executive verdict

No P0 source blocker was found in the inert WakeIntent v0 primitive.

The implementation has the intended fail-closed core:

- one application-fixed scope and one lifetime acceptance per database;
- atomic acceptance and lifetime accounting in one SQLite transaction;
- immutable identity, source, acceptance time, and deadline;
- one terminal transition to `fired`, `revoked`, or `manual_review`;
- no deletion or lifetime-slot recycling;
- duplicate acceptance preserves the first deadline;
- startup and worker events reconcile durable deadlines without polling;
- firing is only a durable observation and creates no Task, provider permit,
  provider invocation, callback, shell, network action, or external effect;
- the sole main worker performs Runtime and SQLite operations; control and signal
  threads only enqueue, publish atomics, and wake/stop the scheduler.

Before a first production wake can be considered, P1 proof and observability gaps
below should be closed or explicitly accepted by Sol. They do not justify enabling
the capability now.

## Security objectives and assets

1. **Authority:** only an explicit local operator action can convert one eligible,
   canonical, succeeded `cognition.reflect.v1` proposal into a wake.
2. **Durability:** a committed deadline survives crash, restart, service
   reinstallation, and host downtime.
3. **At-most-once terminalization:** a wake reaches one terminal state once.
4. **Boundedness:** one accepted wake consumes the fixed scope's lifetime slot
   permanently, including after revocation or manual review.
5. **Exact scheduling:** `due_at` is the persisted UTC lower bound; lateness is
   observable and early firing is forbidden.
6. **Provider separation:** acceptance, scheduling, reconciliation, firing,
   revocation, and inspection never consume or create provider work.
7. **Recovery:** uncertainty fails to durable `manual_review`, and humans retain
   a stopped-service rollback path.
8. **Ownership:** one process owns the database through `state.db.lock`; only the
   worker mutates Runtime/SQLite.
9. **No storm:** no periodic tick, automatic successor, row recycling, or repeated
   terminal event exists.

## Trust boundaries

| Boundary | Untrusted or failure-prone input | Enforced boundary |
|---|---|---|
| Provider result → Task | model text and response shape | bounded-reflection normalization and durable succeeded Task |
| Task → wake authority | foreign kind/status/content, malformed or noncanonical JSON, arbitrary delay | `ExplicitWake::accept` strict source validation; fixed scope, ID, and 900–86400 s delay |
| AF_UNIX → worker | malformed commands, IDs, and reasons | 0600 socket, bounded JSON protocol, mailbox; worker performs the mutation |
| Worker → SQLite | duplicate/replayed/concurrent acceptance | `BEGIN IMMEDIATE`, unique source, per-scope row count, constraints and triggers |
| SQLite → scheduler | restart, overdue row, revoked row, clock discontinuity | reconciliation plus exact earliest durable deadline |
| Scheduler → worker | stale or spurious in-memory event | durable state is recomputed; terminal rows cannot transition again |
| Wake → other work | accidental cognition loop or provider effect | firing only changes the wake row; no successor path is registered |

A privileged host user able to replace binaries or edit a stopped database is
outside the v0 adversary model. Such intervention remains a human recovery action,
not an automatic trust path.

## Durable state model

`armed` is deliberately **not** a durable status. The durable state is
`scheduled`; in-memory arming is derived from the earliest scheduled deadline on
every startup and worker event.

| From | Event | To | Slot |
|---|---|---|---|
| no row | eligible explicit acceptance commits | `scheduled` | permanently consumed |
| `scheduled` | safe observation at/after `due_at` | `fired` | consumed |
| `scheduled` | explicit revoke strictly before `due_at` | `revoked` | consumed |
| `scheduled` | observed clock before `accepted_at` | `manual_review` | consumed |
| any terminal state | any replay/reconcile/revoke | unchanged | consumed |

SQLite triggers reject deadline/source/identity mutation, a second terminal
transition, terminal insertion, and row deletion.

## Adversarial scenario matrix

| ID | Scenario | Expected durable result | Current evidence | Gap / priority |
|---|---|---|---|---|
| W01 | Same source accepted repeatedly before or after terminalization | original row returned; original deadline unchanged | Core and Agent duplicate tests | none |
| W02 | Different source after the one slot is used | `total_exhausted`; no row | Agent/Core lifetime tests | none |
| W03 | Revocation followed by another source | slot stays exhausted | Agent/Core revocation tests | none |
| W04 | Concurrent connections race for the lifetime slot | exactly one acceptance; peer sees exhaustion or duplicate | SQLite two-connection test | none |
| W05 | Missing, pending, failed, foreign-kind, stop, noncanonical, unknown-field, or out-of-range source | no wake row | Agent negative source tests | negative matrix is not exhaustive; P2 |
| W06 | Crash before acceptance commit | no row; same command may be retried | SQLite atomic transaction contract | add explicit failure-injection test; P1 |
| W07 | Crash after commit but before reply or scheduler refresh | `scheduled`; restart re-arms exact deadline | scheduled-before-controller and reopen tests | add full live-control crash-window test; P1 |
| W08 | Crash at/after due before terminal commit | overdue `scheduled`; restart fires once | Core reopen/overdue test | none |
| W09 | Crash after `fired` commit | `fired`; no repeat | reconcile/reopen tests | none |
| W10 | Host is down across deadline | first safe restart observation records `fired_at >= due_at` | overdue reopen test | none |
| W11 | Wall clock jumps forward | all now-due rows fire once | fake-clock reconciliation tests | scheduler-level clock-jump test absent; P1 |
| W12 | Clock moves before `accepted_at` | `manual_review(clock_rollback)`; never armed/fired | runtime and SQLite tests | none |
| W13 | Clock moves backward but remains after `accepted_at` | no early fire; deadline may be delayed | implementation only detects W12 | semantics/telemetry decision required; P1 |
| W14 | Revoke strictly before due | `revoked`; stale arm causes at most one inert event | store and controller tests | none |
| W15 | Revoke is queued before due but processed at/after due | firing wins deterministically | store due-order tests | add mailbox timing test; P1 |
| W16 | Accept/revoke, SIGTERM, and due meet | one committed ordering; restart reconciles it | clean future-wake shutdown test | combined signal/control race test absent; P1 |
| W17 | A bounded handler is still running at `due_at` | no concurrent DB mutation; fire at first later worker observation | controller design | add handler-crosses-deadline test; P1 |
| W18 | Wake and Task lease deadlines coexist | scheduler holds exact minimum; wake reconciles before lease recovery on equal event | wake-earlier-than-lease test | add lease-earlier and equal-deadline cases; P2 |
| W19 | Revoked deadline remains in scheduler | one inert event, then no re-arm | controller stale-deadline test | none |
| W20 | No work and no durable deadline | scheduler blocks indefinitely; no polling | idle controller tests | none |
| W21 | Several generic Core intents become due together | all due rows terminalize atomically; no callback order exists | Core two-intent reconciliation | Agent v0 exposes max one; P2 |
| W22 | Control client disconnects after commit but before reply | durable state wins; retry is duplicate | exception path forces reconciliation | targeted socket test absent; P2 |
| W23 | A client connects and never finishes its AF_UNIX request | worker/scheduler continue; later control commands can be delayed | single server thread, bounded payload only | add I/O timeout or operational recovery proof before effects; P2 for inert v0 |
| W24 | Wake lifecycle runs with existing provider history and nonzero budget | all budget rows and historical Tasks stay unchanged | tests use zero consumption; migration proof preserves rows | production-like nonzero fixture required; P1 |
| W25 | Wake fires while unrelated pending Tasks exist | wake creates nothing; normal dispatcher may process already-authorized work | controller order is reconcile → recover → dispatch | first-wake gate must require quiescent Task/Action state; P1 |
| W26 | Operator loses the known wake/source ID after restart | one durable row remains but live inspection requires its ID | `wake WAKE_ID` only | add fixed-scope status/list recovery view or mandate durable ID record; P1 |
| W27 | Scheduled row is persisted but in-memory arming fails | row survives and restart can retry | startup reconciliation tests | live control cannot observe scheduler arming; P1 |
| W28 | Corrupt/out-of-range persisted timestamps | process should fail closed, never fire early | constraints plus record validation | explicit corruption/time-range tests absent; P2 |
| W29 | Manual-review record is encountered | no automatic resume, delete, or slot refund | terminal-state logic | document stopped-service recovery only; none |
| W30 | Future code attaches an effect to `fired` | current replay safety would be insufficient | explicitly excluded by design | new durable effect marker and ambiguity policy mandatory; future P0 fence |

## Findings by priority

### P0 — blockers

None in the current inert, disabled source primitive.

This verdict does **not** make production schema v4, installed
`--wake-intents`, or a real acceptance ready. Any provider call, successor Task,
external effect, arbitrary timestamp, raised lifetime limit, or automatic resume
would be a new P0 design boundary.

### P1 — close before the first production wake gate

1. **Full crash-window proof:** inject process loss after acceptance commit and
   before reply/deadline refresh, then restart the complete controller and prove the
   exact immutable deadline is re-armed.
2. **Signal/order proof:** cover accept/revoke/SIGTERM/due combinations through the
   mailbox and main loop, including a command queued before due but processed after
   due.
3. **Busy-worker lateness proof:** keep the sole worker in bounded work across
   `due_at`; prove no second mutator, no early terminalization, and one later
   `fired` transition with measurable lateness.
4. **Clock-discontinuity decision:** test a forward jump at scheduler level and
   decide whether a backward jump that remains after `accepted_at` is accepted as
   safe delay or must enter `manual_review`. Current behavior is safe from early
   firing but not fully observable.
5. **Production-like invariant fixture:** begin with the three historical provider
   consumptions, historical Tasks/results/metadata, and zero nonterminal work; prove
   byte-equivalent budget rows and no new Task/Action/provider effect over
   accept → restart → fire/revoke.
6. **Recovery observability:** provide or explicitly waive a read-only fixed-scope
   status that can discover the sole record and distinguish durable `scheduled`
   from the scheduler's currently derived next deadline. Do not create a second
   mutator or polling endpoint.
7. **Quiescence gate:** require zero nonterminal Tasks and Actions immediately
   before real acceptance and keep task/provider submission outside the proof
   window. This avoids attributing ordinary dispatcher work to the wake event.

These are test, observability, and gate-hardening items. No runtime correction is
recommended before Sol chooses their exact scope.

### P2 — useful hardening

- expand the source-shape negative table to malformed JSON, missing result, wrong
  schema, empty/oversized reason, signed/unsigned/floating delay variants, and both
  inclusive bounds;
- test lease-earlier-than-wake and exactly equal lease/wake deadlines;
- test disconnect-after-commit and a non-terminating local control client;
- test malformed/corrupt timestamp records and system-clock range limits;
- retain generic multi-scope/multi-intent tests even though Agent v0 exposes one
  fixed scope and one row.

## Required pre-production proof sequence

A future `wake-prod-01` validator should fail closed unless every step succeeds:

1. explicit Sol + Bertrand authorization names the exact source Task and exact
   Agent/Core/image IDs;
2. production is already schema v4 through its separate approved migration gate;
3. installed profile hash is unchanged except for a separately reviewed explicit
   `--wake-intents` activation, and the service reports the fixed scope/max-total
   line;
4. the source Task is succeeded, canonical `propose_wake`, immutable, and within
   900–86400 seconds;
5. Tasks and Actions are quiescent; provider budget rows and historical Task reports
   are snapshotted;
6. exactly one `accept-wake SOURCE_TASK_ID` command is issued;
7. durable acceptance is inspected, including exact `accepted_at_ms` and
   `due_at_ms`; duplicate replay consumes no new slot and changes no deadline;
8. a controlled restart before due proves exact durable re-arm;
9. either explicit pre-due revocation or one inert due transition is observed;
10. no successor Task/Action, provider consumption, external effect, polling loop,
    TCP listener, or second wake appears;
11. budget rows and historical state match the preflight snapshot;
12. Fedora shutdown/restart and the stopped-service rollback path remain safe.

The validator must never submit a provider Task or fabricate a source proposal.

## Recommended follow-up split

- **Test-only PR:** W06/W07, W11, W15–W18, and W24/W25.
- **Observability design review:** W26/W27, with no new writer and no polling.
- **Future gate document:** exact `wake-prod-01` operator procedure after schema-v4
  production and wake-off profile gates succeed.
- **Deferred hardening:** P2 cases, unless a P1 test exposes a source defect.

The current production configuration must continue to omit `--wake-intents`.
