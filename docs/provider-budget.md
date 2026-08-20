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

## Crash semantics

Budget consumption is conservative. Once a new permit is accepted, a later crash does not refund it.

The permit uses the same deterministic provider Action idempotency key. If the process dies after the budget record is committed but before the Action is created, a replacement process sees the same budget consumption as a duplicate permit and may continue creating that one Action. It does not consume a second slot.

Once an Action already exists, the existing provider no-replay rules take precedence and no additional budget slot is consumed by reconciliation.

## Clock behavior

Rolling windows and cooldowns use wall-clock time because the state must survive process and machine restarts. If the observed clock moves backwards behind the most recently consumed permit, the budget fails closed and the task enters manual review instead of authorizing a call.

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
