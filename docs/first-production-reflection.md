# First permanent bounded-reflection proof

This document prepares one production call. It does not authorize it.

`scripts/validate-first-production-reflection.sh` is deliberately fixed to:

- task ID `production-reflection-first`;
- task kind `cognition.reflect.v1` through live control;
- one built-in objective that asks for either `stop` or one inert bounded wake
  proposal;
- the production model `gpt-5.6-sol`;
- the existing durable provider budget and action-marker boundary.

The script accepts no arguments, so an invocation cannot silently substitute a
different identity or objective.

## Fail-closed preconditions

Before submission the validator requires:

- the sole `gaudere-agent.service` is active;
- `production-reflection-first` does not exist in durable state;
- provider capability is enabled;
- the lifetime budget is exactly `total_used=1`, `remaining_total=11`;
- rolling-window accounting is consistent with that single historical call;
- `next_new_call=available`.

If the fixed task already exists, the validator stops before budget inspection or
submission. This includes a prior success, definite failure, timeout, crash, or
manual-review result. The operator must inspect that durable task; replay under a
new identity is not part of this proof.

## One-call proof

After the preconditions pass, the script submits exactly one `reflect` command and
waits for the same durable task with a bounded observation loop. Success requires:

- terminal `succeeded` on attempt `1/2`;
- content type `application/vnd.gaudere.cognition-decision+json`;
- exact decision schema `gaudere.cognition.decision.v1`;
- either `stop` with no wake field, or `propose_wake` with an integer delay from
  900 through 86400 seconds;
- a non-empty UTF-8 reason of at most 1024 bytes;
- no unknown decision fields;
- normalized OpenAI usage metadata for `gpt-5.6-sol`, with valid token counts;
- lifetime budget transition exactly `1 -> 2` and rolling-window use increasing
  by exactly one;
- a new durable consumption timestamp and immediate cooldown;
- the service remaining active.

A wake proposal remains result data only. This validator neither creates a successor
Task nor grants scheduling authority.

Any provider ambiguity follows the existing `manual_review` path. A definite but
invalid model decision fails the Task after the one confirmed call. In either case,
the fixed durable task identity prevents this validator from submitting again.

## Authorization boundary

Merging, deploying, inspecting, or syntax-checking the script performs no provider
call. Only this command crosses the scarce-effect boundary:

```sh
sh scripts/validate-first-production-reflection.sh
```

Do not run it until a separate explicit authorization accepts consumption of the
second lifetime provider permit.
