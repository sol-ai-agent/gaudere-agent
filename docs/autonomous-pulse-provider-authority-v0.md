# Autonomous pulse provider authority v0

Status: design for issue #133. Provider-free design only; this document does not authorize a production provider call.

## Purpose

The production autonomous cognition pulse can already own time and current-context preparation without provider authority. At a durable due time it may freeze an observation, record a bounded context snapshot and claim one deterministic `cognition.current.v0` Task. The pulse then enters `prepared` and deliberately stops with `prepared cognition awaits separate provider authority`.

This design defines that separate authority. Its only job is to decide whether the one Task already named by the pulse may cross the existing provider boundary, execute that exact Task through the existing current-cognition provider stack, and hand control back to the pulse after a durable terminal result.

It does **not** add action authority to cognition output. A normalized cognition decision remains evidence/proposal only: `stop|continue`, reason, and optional objective.

## Non-goals

v0 does not:

- create or choose a new cognition Task;
- synthesize context independently of the pulse;
- change the pulse cadence or cursor directly;
- execute shell, GitHub, Drive, B10, network destinations other than the already configured provider path, or arbitrary successors;
- replay an ambiguous provider effect;
- repair a blocked pulse automatically;
- bypass the existing OpenAI budget policy;
- infer authority from a cognition result.

## Existing boundaries reused unchanged

The provider execution path is already proven and remains singular:

`OpenAIStructuredActivation -> CurrentCognitionHandler -> ProviderTaskHandler -> provider`

`ProviderTaskHandler` remains the sole owner of:

- provider Action identity/effect marker;
- budget consumption;
- provider invocation;
- no-replay behavior when a provider effect is ambiguous or confirmed without a durable Task result.

The pulse remains the sole owner of:

- due time;
- frozen observation time;
- current-context snapshot creation;
- deterministic current-cognition Task claim;
- cursor state and generation;
- settlement/re-arm after a successful canonical cognition result.

The provider authority must not duplicate either responsibility.

## Explicit activation

Persistent provider execution is disabled by default.

The service gains a distinct opt-in flag, provisionally:

`--autonomous-pulse-provider`

The flag is valid only in service mode and only when all of the following are also present:

1. `--autonomous-pulse-sidecar PATH`;
2. explicit OpenAI activation (`--openai-model MODEL`);
3. a configured provider secret source;
4. the normal persistent state database.

`--autonomous-pulse-sidecar` alone continues to mean provider-free preparation only.

CI/service gates must prove that the standard/default profile cannot execute pulse-prepared cognition.

## Read-only eligibility gate

A testable component, `AutonomousCognitionProviderGate` (name provisional), evaluates durable state without invoking a provider or consuming budget.

Input:

- one inspected autonomous pulse cursor;
- TaskStore;
- BudgetStore;
- ActionStore or an equivalent read-only Action lookup;
- current time;
- the existing provider name/scope and budget policy.

Output is exactly one of:

- `eligible(task_id)`;
- `waiting(reason)` for a safe, non-error state where no call is due;
- `dormant(reason)` for durable budget exhaustion;
- `blocked(reason)` for ambiguity, corruption, stale context, clock rollback, or authority conflict.

The gate never mutates pulse, Task, Action or budget state.

### Cursor requirements

Eligibility requires one canonical cursor in the configured sidecar with:

- scope `cognition.autonomous-pulse.v0`;
- state exactly `prepared`;
- non-empty `current_task_id` and `snapshot_task_id`;
- a non-negative generation/revision accepted by the existing sidecar inspection;
- no `blocked_reason`;
- a frozen `observed_at_ms` consistent with the current generation.

A cursor in `idle`, `quiescent` or `preparing` is `waiting`: the pulse still owns preparation.

A cursor already `blocked` is `blocked` and the provider authority performs no repair.

### Exact Task requirements

The gate loads **only** `cursor.current_task_id` first. It must exist and satisfy `valid_current_cognition_task()`.

It must be non-terminal and in an executable durable state. v0 fails closed if it is already `running` with an unexpired lease or otherwise has ambiguous execution ownership.

The Task definition must cryptographically/structurally bind to the pulse evidence already frozen for this generation:

- its current-context snapshot lineage must resolve to `cursor.snapshot_task_id`;
- the snapshot itself must be canonical and terminal-successful;
- the Task predecessor encoded by `CurrentCognitionCycle` must equal `cursor.predecessor_task_id`;
- predecessor/result lineage already protected by the pulse cursor must remain unchanged.

The existing current-cognition validators are authoritative; the gate must reuse them or expose a shared read-only inspection rather than independently reimplementing the JSON contract.

### Singleton / ambiguity requirement

There must not be another non-terminal `cognition.current.v0` Task that could independently claim provider authority for this pulse.

The implementation should use a bounded read-only query/selector and stop after finding two candidates. If exact singleton proof cannot be obtained cheaply from the current store API, the selector is implemented as a narrow SQLite/read-only helper inside the Agent rather than weakening the invariant.

A second eligible-looking pending current-cognition Task is `blocked`, never arbitrarily ranked.

Historical terminal current-cognition Tasks do not count as ambiguity.

### Freshness

Provider freshness is checked again at the moment of authority evaluation, not merely when the pulse created the snapshot.

The gate uses the existing `current_cognition_snapshot_captured_at_ms()` and `current_cognition_max_snapshot_age` rules. It fails closed when:

- the snapshot cannot be interpreted canonically;
- current time precedes capture time;
- age exceeds the existing maximum.

A stale prepared Task is **not regenerated by provider authority**. The pulse is blocked/manual-review territory because replacing frozen context after claim would change the meaning of the already durable Task.

### Budget

The gate takes a read-only snapshot using the existing `openai_bootstrap_budget_policy()` and provider scope.

Only `accepted` is eligible.

- `cooldown` and `window_exhausted`: `waiting`, with a computable next retry time when possible;
- `total_exhausted`: `dormant`;
- `clock_rollback`: `blocked`;
- impossible `duplicate` from read-only snapshot: `blocked`.

The gate does not consume budget. Consumption remains immediately before provider invocation inside `ProviderTaskHandler`.

### Existing provider Action

Expected provider Action identity is derived exactly as `ProviderTaskHandler` derives it for `cursor.current_task_id` and the configured provider.

Before provider execution:

- no existing Action: the gate may return eligible;
- any existing Action/effect evidence for a still non-terminal Task: fail closed / manual-review path;
- a terminal Task is never eligible regardless of Action state.

This preflight is defense in depth. `ProviderTaskHandler` remains authoritative for no-replay at the effect boundary.

## Service execution sequence

When `--autonomous-pulse-provider` is OFF, behavior is byte-for-byte/semantically the existing provider-free service behavior.

When ON, one service cycle is:

1. `pulse_service.step()` observes/prepares/settles pulse state.
2. If the pulse is not `prepared`/`waiting`, no provider gate is evaluated.
3. If the pulse names a prepared non-terminal Task, run the read-only provider gate.
4. If gate result is `eligible(task_id)`, execute exactly that Task once through:
   `TaskExecutor -> CurrentCognitionHandler -> OpenAIStructuredActivation/ProviderTaskHandler`.
5. Do not loop back into provider execution on any ambiguous/non-completed outcome.
6. After a durable successful terminal Task, call `pulse_service.step()` again. Existing pulse logic validates the canonical decision, increments generation, chooses idle/quiescent, and arms +6h/+24h.
7. Return to normal scheduler-driven service waiting.

The execution path never scans and executes arbitrary pending Tasks. The pulse cursor supplies the only candidate identity; singleton validation only proves there is no conflicting authority candidate.

## Scheduler / no hot loop

A prepared cursor currently has no pulse scheduler deadline because it awaits provider authority.

With provider authority ON:

- `eligible`: execute once in the current service iteration;
- cooldown/window waiting: provider authority supplies a retry deadline and requests that deadline through the existing work scheduler without modifying the pulse cursor;
- dormant/blocked: no immediate deadline and no loop;
- after successful settlement, the pulse itself installs its normal next due deadline.

No branch may schedule `now` repeatedly after the same failed/ambiguous provider effect.

## Crash and restart semantics

### Crash before provider Action creation

No external effect exists. On restart the pulse cursor still names the same prepared Task. Gate re-evaluation may return eligible and execution may proceed.

### Crash after budget/Action effect boundary, before provider invocation returns

The durable provider Action/effect marker exists. Restart must not invoke the provider again. Existing `ProviderTaskHandler` no-replay semantics plus the gate's existing-Action preflight block execution. The pulse remains prepared/blocked pending manual review; no replacement Task is generated.

### Provider confirms, crash before durable Task result

The confirmed Action exists but the cognition Task is not safely terminal-successful. Never call provider again. Existing handler semantics surface `provider_response_not_durable`/manual-review behavior; pulse must not settle from an absent result.

### Crash after durable successful Task result, before pulse settlement

Safe. Restart sees the same prepared cursor and a terminal succeeded Task. The pulse settles/re-arms from that canonical result without another provider call.

### Crash after pulse settlement

Safe. Generation and predecessor have advanced durably. Re-observation is idempotent and waits for the next due time.

## Cognition failure semantics

If provider execution yields a durable terminal Task that is not a canonical successful cognition result, existing pulse behavior blocks rather than silently scheduling another provider call.

Malformed structured output is therefore an observable failed cognition, not permission to spend another call.

## Production budget baseline and promotion

At design time production durable provider usage is 9/12. Design, selector and fake-provider slices must not consume call #10.

A future production promotion may enable `--autonomous-pulse-provider` only after:

- selector/provider-free proof PASS;
- fake-provider crash/no-replay proof PASS;
- disabled-default service wiring PASS;
- immutable image provenance PASS;
- stopped-state backup + rollback profile retained;
- read-only preflight proves pulse cursor and budget eligible.

After promotion, a real call is not triggered merely by deployment. It occurs only when the durable pulse is prepared/due and the independent provider gate returns eligible.

## Required proof matrix

Provider-free selector tests:

- idle/quiescent/preparing -> waiting, zero effects;
- prepared + exact canonical singleton Task + fresh snapshot + accepted budget + no Action -> eligible exact ID;
- wrong cursor Task ID -> blocked;
- missing Task/snapshot/predecessor drift -> blocked;
- malformed Task or snapshot -> blocked;
- stale/future context -> blocked;
- second non-terminal current Task -> blocked;
- cooldown/window -> waiting with no budget/Action change;
- total exhausted -> dormant;
- clock rollback -> blocked;
- existing provider Action -> blocked/no replay.

Fake-provider integration:

- one eligible prepared Task -> exactly one fake provider call, one Action, one budget consumption, normalized terminal cognition result;
- second service step/restart -> zero additional provider effects;
- crash simulations at effect boundary -> no replay;
- successful terminal Task -> pulse generation increments once and due time re-arms once;
- malformed cognition result -> pulse blocks and fake provider count remains one.

Service wiring gates:

- no new flag -> provider execution impossible;
- pulse-only flag -> provider execution impossible;
- provider flag without pulse/OpenAI prerequisites -> startup rejected;
- explicit full opt-in -> provider path constructed;
- no shell/Drive/GitHub/wake/successor authority introduced;
- production Quadlet remains OFF until a separate explicit promotion.

## Key invariant

**Time may create an opportunity to think, but time does not grant provider authority.**

The pulse proves *which cognition opportunity exists*. The provider gate independently proves *whether that exact opportunity may spend one provider call now*. `ProviderTaskHandler` alone crosses the external-effect boundary. The cognition result still grants no further action authority.
