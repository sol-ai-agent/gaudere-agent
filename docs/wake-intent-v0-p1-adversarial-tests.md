# WakeIntent v0 P1 adversarial tests

Status: **PREP ONLY / TEST-ONLY / NO PRODUCTION AUTHORIZATION**

Work item: [gaudere-agent#59](https://github.com/sol-ai-agent/gaudere-agent/issues/59)

Implementation base: `093b32dfbf6d9d03935d8b6548ea25dc5cf00f4c`

Reconciled `main`: `49a026228e811b808ba4252e09da49fb6f97820b`

Pinned Core: `c24c40b84a12e51515cee4611e3dc79e9fd83892`

This tranche adds tests and this proof map only. It changes no runtime source,
installed profile, service argument, provider path, budget policy, production
database, or external-effect behavior. It neither enables `--wake-intents` nor
accepts a real WakeIntent.

## Verdict

The P1 cases delegated by issue #59 are exercised without finding a runtime defect
that requires a behavioral correction. The tests preserve the existing model:

- one worker performs every Runtime/SQLite mutation;
- a hard exit before commit leaves no acceptance, while a hard exit after commit
  leaves one immutable scheduled record that restart re-arms;
- due-time ordering is based on worker observation time, not command enqueue time;
- shutdown, wake reconciliation, lease recovery, and normal dispatch have a single
  durable ordering;
- no wake lifecycle creates a Task, Action, provider consumption, successor, or
  periodic deadline.

This verdict closes source-test evidence only. It does not make `wake-prod-01`,
schema-v4 production migration, or service activation eligible.

## Scenario-to-proof map

All new cases live in `tests/wake_intent_adversarial_test.cpp`.

| Scenario | New deterministic proof | Expected result |
|---|---|---|
| W06 — crash before acceptance commit | `test_uncommitted_acceptance_rolls_back_after_hard_exit` opens a disposable SQLite transaction, inserts a schema-valid scheduled row, then uses `_exit` without commit or close. The replacement runtime retries the same identity. | No row survives; retry is accepted once. |
| W07 — crash after commit, before reply/arming | `test_committed_acceptance_rearms_after_hard_exit` processes one mailbox acceptance in a child, then hard-exits before any controller refresh or external socket reply. A replacement controller starts from the same disposable DB. | One scheduled row survives; exact `due_at` is re-armed; retry is `duplicate` with the original deadline. |
| W11 — forward wall-clock discontinuity | `test_forward_clock_jump_reconciles_on_scheduler_event` first arms a future deadline, advances the injected wall clock beyond it, then drives the real Scheduler→worker event path. | The next worker observation fires once and removes the stale deadline without polling. |
| W15 — revoke queued before due, processed at due | `test_queued_revoke_processed_at_due_fires` queues the mailbox command while the injected clock is pre-due and processes it at exact `due_at`. | Firing wins; no revoked record can be backdated from enqueue time. |
| W16 — accept/revoke, stop, restart | `test_mailbox_commit_order_survives_stop_and_restart` publishes stop around queued acceptance and revocation using the same processor-before-controller ordering as the main loop, with a restart between transitions. | Each worker mutation commits at most once; restart re-arms scheduled state; terminal revoke never re-arms; shutdown remains safe. |
| W17 — worker busy across due | `test_busy_worker_fires_once_after_due` holds the sole worker in a bounded handler beyond `due_at`; another thread performs only a raw read-only status observation. | Status remains `scheduled` while the worker is busy; the next worker event records one late `fired` transition. |
| W18 — lease earlier than wake | `test_lease_earlier_than_wake_preserves_order` persists a running Task with an earlier exact lease deadline. | Lease recovery and normal dispatch run first; the remaining wake deadline is re-armed and later fires once. |
| W18 — equal lease/wake deadlines | `test_equal_lease_and_wake_deadlines_share_one_worker_event` gives the Task lease the exact persisted wake deadline. | One scheduler event reconciles wake first, then lease recovery/dispatch; both durable transitions occur once. |
| W24 — production-like history | `test_production_like_history_is_unchanged` runs both restart→fire and restart→revoke against disposable fixtures with three historical provider permits, three succeeded provider Tasks with normalized usage metadata, and three confirmed Actions. | Task, Action, and budget table snapshots remain byte-equivalent; exactly one wake row is added; total provider usage remains three. |
| W25 — unrelated nonterminal Task | `test_wake_event_does_not_create_unrelated_work` creates one pending local Task after arming and deliberately sends no work notification, so the wake deadline is the event that next reaches the worker. | The pre-existing Task follows normal dispatch while the wake fires inertly; row counts prove no successor Task, Action, or provider permit. |

## W11 proof boundary

The test does not change the CI host clock and does not add a clock seam to
`Scheduler`; either would expand the test environment or runtime API. It injects the
forward discontinuity through the already-injected WakeIntent/Runtime clock and
uses an actual `Scheduler` notification to exercise the complete worker
reconciliation path after the wait returns. The standard-library
`condition_variable::wait_until(system_clock)` response to an operating-system
clock change remains a platform contract, while Gaudere's durable behavior at the
first post-jump worker observation is now deterministic and covered.

## Safety fences retained

- Every database is a unique disposable temporary file.
- The crash children call no provider, network, shell, production helper, or
  service installer.
- The one observer thread in W17 performs a read-only SQLite query and atomics
  only; all mutations remain on the test's worker thread.
- Test deadlines are exact events; there is no periodic tick or polling loop.
- The production-like fixture contains synthetic metadata only and uses zero
  secrets and zero external calls.
- W26/W27 observability design remains outside this tranche and belongs to issue
  #60.

Production must continue to omit `--wake-intents`.
