# Resume-after-wake v0 threat and crash matrix

Status: **DESIGN ONLY / NO PROVIDER AUTHORITY**  
Parent: #84  
Provider-free proof: #85  
Future real gate: #86

## Safety objective

A durable `fired` WakeIntent may eventually authorize exactly one bounded cognition continuation, but only through a separate capability and a durable deterministic Task claim. WakeIntent itself remains inert.

The proof obligation is stronger than simple deduplication: every crash point must have an unambiguous durable interpretation, and any point where a provider effect may have occurred must forbid blind replay.

## Assets and boundaries

| Asset/boundary | Required property |
|---|---|
| Wake row | immutable evidence only; never modified by resume |
| Source Task/result | canonical succeeded `cognition.reflect.v1` + `propose_wake` lineage |
| Resume Task | one deterministic durable authority claim per wake |
| Provider budget | consumed before invocation; deterministic key; no bypass |
| Provider Action | durable external-effect marker and ambiguity state |
| Resume result | bounded normalized proposal only; no automatic external action |
| Production profile | resume disabled unless separately activated |
| Host | no poweroff/reboot/logout/network/service disruption required |

## State derivation

There is no separate resume state table in v0. Resume state is derived from wake + deterministic Task + provider Action:

| Derived state | Durable evidence |
|---|---|
| disabled | resume component/capability absent |
| ineligible | wake/source fails strict validation |
| eligible | valid fired lineage; deterministic Task absent |
| claimed | deterministic Task present pending/running |
| completed | deterministic Task succeeded with canonical resume decision |
| failed | deterministic Task terminal failed/cancelled before ambiguous effect |
| manual_review | Task manual-review or provider Action effect ambiguous/response unavailable |

A status implementation must never repair state as a side effect.

## Crash/adversarial matrix

| ID | Scenario | Expected durable result | Automatic replay? | Proof target |
|---|---|---|---|---|
| R01 | Resume capability absent, wake fires | wake remains fired; no resume Task/Action/budget change | no | provider-free disabled test |
| R02 | Capability present but wake still scheduled | no resume Task | no | negative eligibility |
| R03 | Wake revoked/manual_review | no resume Task | no | negative eligibility |
| R04 | Foreign scope or source kind/status/result | no resume Task | no | negative lineage matrix |
| R05 | Source result malformed/noncanonical/not `propose_wake` | no resume Task | no | negative schema matrix |
| R06 | Wake/source identity mismatch | no resume Task; derived status ineligible | no | corruption fixture |
| R07 | Crash after reading eligible wake but before Task submit | no claim exists | yes, safe to re-evaluate | synthetic crash point |
| R08 | Crash during Task validation before save | no Task | yes | synthetic crash point |
| R09 | Task save commits, process dies before notify/reply | one pending deterministic Task | yes only as duplicate observation; never create distinct Task | reopen test |
| R10 | Same wake reconciled repeatedly | same Task ID/key; submit reports duplicate | yes, identity-only | duplicate loop test |
| R11 | Restart with pending resume Task | pending Task remains; no second claim | normal dispatcher may continue it | reopen test |
| R12 | Conflicting Task ID exists with different idempotency/definition | fail closed/manual review or explicit conflict; do not overwrite | no | conflict fixture |
| R13 | Conflicting idempotency key exists under different Task ID | fail closed; do not create alternate identity | no | conflict fixture |
| R14 | Two resume reconciliation events in same one-process worker | one Task accepted, later duplicate | yes | repeated-event test |
| R15 | Future multi-process writers race | current design no longer sufficient | no | explicit unsupported fence |
| R16 | Resume Task lease begins, crash before provider budget consume | Task lease eventually recovers; no provider effect exists | bounded retry may proceed | fake handler crash test |
| R17 | Budget consume commits, crash before provider Action submit | same budget key is durable duplicate; no provider invocation happened | yes with same key, then Action may be submitted | existing provider invariant + targeted integration test |
| R18 | Provider Action submitted pending, crash before start | existing Action is evidence of prior slice; current ProviderTaskHandler conservatively refuses replay and requests manual review | no | provider fake/integration |
| R19 | Action running, crash before `effect_started` | current handler conservatively manual-reviews any existing Action | no | provider fake/integration |
| R20 | `effect_started` commits, crash before provider invocation | effect may or may not have started from recovery perspective | no, manual review | provider invariant |
| R21 | Provider invocation in flight, process dies | external effect unknown | no, manual review | provider invariant |
| R22 | Transport returns ambiguous outcome | Action effect unknown/manual review; budget remains consumed | no | existing provider tests |
| R23 | Provider succeeds, confirmation persistence fails | success may have occurred but durable confirmation absent/ambiguous | no | provider fake fault injection |
| R24 | Confirmation persists, crash before Task result persists | Action confirmed but response not durable; existing handler returns manual review rather than recall provider | no | targeted integration test |
| R25 | Task success result persists, reply/notification lost | completed Task/result durable | duplicate read only | reopen/status test |
| R26 | Resume result invalid JSON/schema after provider success | provider effect confirmed, Task fails normalization; no second provider call | no | normalizer negative test |
| R27 | Resume result says `continue` with oversized/invalid objective | fail normalization; provider not replayed | no | normalizer bounds |
| R28 | Resume result contains URL/shell/tool/action fields | unknown keys rejected | no | schema negative test |
| R29 | Provider lifetime/window/cooldown denies call | no provider invocation; current handler produces terminal failure | no automatic second resume for same lineage | first-real gate must preflight budget |
| R30 | Clock rollback affects provider budget | manual review from provider budget layer | no | inherited provider proof |
| R31 | Unrelated pending Tasks coexist | first real gate may require quiescence; resume must not mutate unrelated Tasks | n/a | provider-free invariant snapshot |
| R32 | Wake row changes after resume claim | WakeIntent immutability should reject mutation; resume Task identity remains bound to original lineage | no | store invariant |
| R33 | Resume Task is cancelled before provider effect marker | cancellation may finish without provider invocation | no extra resume Task | handler test |
| R34 | Cancellation after provider effect marker | effect uncertainty dominates; manual review/no replay | no | provider boundary test |
| R35 | Operator deletes/recycles evidence manually | outside automatic model; stop/manual recovery only | no | documented boundary |
| R36 | Successful resume cognition automatically executes objective | forbidden in v0; no executor registered | no | architecture/static test |
| R37 | Successful resume cognition automatically accepts another wake | forbidden; no implicit WakeIntent extension | no | architecture/static test |
| R38 | Status inspection causes Task/Action/budget mutation | forbidden | no | read-only status test |
| R39 | Capability toggled off after Task already claimed | existing durable Task must not be silently deleted; activation/rollback procedure must define whether dispatcher is allowed to finish or service is stopped for review | no implicit deletion | future profile gate |
| R40 | Runtime starts with fired eligible wake and capability enabled | future automatic reconciler may create deterministic Task once, never call provider directly | yes at claim layer only | later disabled-by-default integration |

## Critical conclusions

### 1. No new resume table is necessary for the first boundary

The deterministic Task is already a durable claim object with ID, idempotency key, lifecycle, result, attempt count and lease recovery. A new `resume_claims` table would duplicate these properties without removing the provider ambiguity problem.

The no-new-table decision depends on the current one-process/one-mutator model. It must be reconsidered if multiple writers are introduced.

### 2. Task submission and provider effect are different boundaries

Task submission is an internal, durable, idempotent effect. It is safe to re-evaluate after a crash because a committed Task is discoverable by deterministic identity and an uncommitted Task has no external consequence.

Provider invocation is different. It can succeed remotely while local persistence fails. Therefore the existing Action `effect_started`/unknown/confirmed states remain mandatory, and any ambiguity forbids replay.

### 3. Conservative existing provider recovery is acceptable for v0

`ProviderTaskHandler` deliberately refuses to replay an existing provider Action even when the durable state suggests the provider may not yet have been invoked. This can sacrifice liveness but protects the no-duplicate-call invariant. Resume v0 should inherit that conservative behavior rather than weakening it.

### 4. Current budget denial semantics are a liveness limitation

Cooldown/window denial currently terminates the Task instead of postponing it. The first real resume gate must therefore establish budget eligibility before claiming the one-shot Task. A future autonomous budget-wait scheduler is separate work and must not be smuggled into #84.

### 5. A resume decision is not an action plan executor

The proposed `continue` result carries a bounded `objective` only. There is no automatic shell/network/tool/Task fan-out from the result. If the experiment later needs Gaudere to act on that objective, a further explicit authority boundary is required.

## Provider-free proof matrix for #85

The first implementation slice should prove at least:

1. capability disabled + fired wake -> zero Task/Action/budget mutation;
2. valid fired lineage -> exact deterministic Task accepted once;
3. 100 repeated reconciliations -> one identical Task;
4. close/reopen store -> same Task and derived status;
5. scheduled/revoked/manual-review wake -> no Task;
6. missing/failed/foreign source -> no Task;
7. malformed/unknown-key/noncanonical source result -> no Task;
8. ID and idempotency conflicts -> fail closed without overwrite;
9. crash hook immediately before submit -> safe retry creates one Task;
10. crash hook immediately after durable save -> reopen sees one Task; no second claim;
11. snapshots prove provider budget/history and unrelated Tasks/Actions unchanged;
12. status view is read-only;
13. static/runtime proof that no provider object is constructed and no network transport is reachable in this test slice.

Provider-free PASS must not require a production image rebuild/deploy. CI/local isolated test evidence is sufficient for #85.

## Later provider-fake proof before #86

Before one real call can be considered, add a fake provider integration that proves R16–R29, especially:

- budget commit before Action/provider effect;
- deterministic budget/Action key from resume Task;
- no replay after any existing Action at or past ambiguous boundary;
- confirmed Action but missing Task output -> manual review, not provider recall;
- invalid resume output -> no recall;
- successful output normalizes exactly to `gaudere.cognition.resume-decision.v1`.

## First-real gate preconditions

#86 remains blocked until all are true:

- #84 design reviewed/merged;
- #85 provider-free PASS;
- provider-fake ambiguity matrix PASS;
- exact source/wake lineage chosen;
- exact Agent/Core/image refs frozen;
- target environment explicitly chosen;
- resume capability disabled by default in ordinary production profile;
- production WakeIntent state and lifetime semantics understood for the target;
- provider budget is eligible and the permanent call is separately authorized;
- unrelated work is quiescent or explicitly accounted for;
- Bertrand is informed before any host-visible action;
- rollback/stopped-service path documented.

## Stop conditions

Any evidence of a second resume Task, second provider Action/key, provider replay after ambiguous effect, hidden downstream action, production profile activation without gate, or provider budget bypass is a P0 failure.
