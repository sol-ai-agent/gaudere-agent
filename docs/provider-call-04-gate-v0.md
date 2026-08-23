# Provider call #4 gate v0

Status: **PREP ONLY**. Merging this gate does not authorize or execute a provider call and does not enable WakeIntent.

## Purpose

Create one durable `cognition.reflect.v1` source Task after the proven pre-wake runtime upgrade. The reflection is deliberately neutral: `stop` is a valid experimental outcome, and `propose_wake` is valid only when Gaudere identifies a self-generated thread worth resuming after a real delay.

Frozen production baseline:

- runtime image: `sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01`
- Agent: `4e6cb09467456f38377bd8610e1ac534c7705380`
- Core: `1316cf68db93e4c91a7bd79fbd289b8f382f8659`
- provider budget before execution: `total_used=3`, `remaining_total=9`
- WakeIntent capability: disabled
- fixed Task ID: `production-reflection-wake-source-first`

## Fail-closed behavior

Before submission the gate requires:

1. explicit argument `--execute-after-explicit-provider-call-04-go`;
2. active `gaudere-agent.service`;
3. the exact immutable runtime image and Agent/Core provenance above;
4. `wake-status` to prove WakeIntent is still disabled;
5. provider capability enabled, exactly three lifetime consumptions, nine remaining, and `next_new_call=available`;
6. the fixed source Task ID to be absent.

The gate then issues exactly one `reflect` submission. If live control reports an error, the gate reconciles by Task inspection only and never resubmits automatically. A timeout, failed Task, manual review, or ambiguous state is a hard stop requiring inspection.

A successful result must already be normalized by the runtime to `application/vnd.gaudere.cognition-decision+json` and be exactly one canonical `gaudere.cognition.decision.v1` decision:

- `stop` with a non-empty reason; or
- `propose_wake` with a non-empty reason and `wake_after_seconds` in `900..86400`.

After success the gate requires `total_used=4`, `remaining_total=8`, WakeIntent still disabled, zero wake effects, and the service still active.

## Important experimental rule

A `stop` result is not a failed experiment and must not be rewritten, retried, or coerced into `propose_wake`. A `propose_wake` result is only a source proposal; it grants no authority to enable WakeIntent or accept a wake. Those remain separate explicit gates.

## Production execution

Only after a separate explicit Sol/Bertrand authorization:

```sh
cd ~/Documents/Codes/Projets/ia/gaudere/gaudere-agent
git switch main
git pull --ff-only
sh scripts/run-provider-call-04-gate-v0.sh --execute-after-explicit-provider-call-04-go
```

Expected terminal proof includes:

```text
decision=stop
```

or:

```text
decision=propose_wake
wake_after_seconds=<900..86400>
```

and in both cases:

```text
canonical_decision=PASS
provider_effects=1
wake_effects=0
wake_capability_active=false
service_final=active
provider_total_after=4
gaudere provider call 04 gate: PASS
```
