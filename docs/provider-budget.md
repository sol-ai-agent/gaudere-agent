# Bootstrap provider call budget

Permanent provider access must not be limited only by the provider account's monetary ceiling. Gaudere also enforces a local durable admission budget before creating a new external provider Action.

## Initial OpenAI policy

The bootstrap OpenAI activation policy is intentionally hard-coded:

- 12 new OpenAI calls total for the durable state;
- at most 4 new calls in a rolling 24-hour window;
- at least 15 minutes between new calls.

The policy is not a command-line option or mutable runtime setting in this phase. Raising it requires a reviewed code change and redeployment.

The separate OpenAI project hard spend limit remains an independent outer guardrail.

## Durable ordering

For a new provider task the worker follows this order:

1. reject cancellation before any scarce resource is consumed;
2. check for an existing provider Action and apply the existing no-replay policy;
3. atomically check and consume the durable provider budget;
4. create and start the recoverable provider Action;
5. durably record `effect_started` immediately before provider invocation;
6. perform the single provider request.

A budget denial therefore occurs before any provider Action and before any network effect.

The budget record is persisted in the same SQLite state database as Tasks and Actions, in the additive `budget_consumptions` table. Normal stopped-state backups and restores therefore preserve consumed permits automatically.

## Live observability

The owner service can report the durable OpenAI budget without a second process opening SQLite:

```sh
sh scripts/control-service.sh budget
```

The request crosses the same mode-`0600` Unix socket as live Task commands. The socket thread only queues and wakes; the main worker performs a non-mutating `BudgetStore::snapshot()` against the already-owned state database.

A report contains the fixed policy, lifetime/window usage, remaining slots, the latest durable consumption timestamp, whether the provider itself is enabled, and the admission result a **brand-new** call would receive at that instant. Example for an unused offline service:

```text
scope="provider.call:openai.responses"
provider_enabled=false
max_total=12
total_used=0
remaining_total=12
max_window=4
window_seconds=86400
in_window_used=0
remaining_window=4
min_interval_seconds=900
last_consumed_at_ms=none
next_new_call=available
```

Possible `next_new_call` values are `available`, `cooldown`, `window_exhausted`, `total_exhausted`, and `clock_rollback`. Observation never consumes or reserves a permit.

## Crash semantics

Budget consumption is conservative. Once a new permit is accepted, a later crash does not refund it.

The permit uses the same deterministic provider Action idempotency key. If the process dies after the budget record is committed but before the Action is created, a replacement process sees the same budget consumption as a duplicate permit and may continue creating that one Action. It does not consume a second slot.

Once an Action already exists, the existing provider no-replay rules take precedence and no additional budget slot is consumed by reconciliation.

## Clock behavior

Rolling windows and cooldowns use wall-clock time because the state must survive process and machine restarts. If the observed clock moves backwards behind the most recently consumed permit, the budget fails closed and the task enters manual review instead of authorizing a call.

The observational snapshot follows the same fail-closed interpretation and reports `next_new_call=clock_rollback` without mutating state.

## Operator-visible failures

A provider task rejected by the local budget reaches a durable terminal state without provider invocation:

- `provider_budget_total_exhausted`
- `provider_budget_window_exhausted`
- `provider_budget_cooldown`

Clock rollback and budget-store failures require manual review:

- `provider_budget_clock_rollback`
- `provider_budget_unavailable`

## Reset boundary

Replacing Gaudere's durable `state.db` with a fresh database also replaces its budget history. That is intentionally treated as a state reset, not as a supported way to replenish quota. Backup, restore, and migration procedures must preserve the whole state database.
