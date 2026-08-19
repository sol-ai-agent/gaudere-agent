# Provider execution boundary

This document describes the provider-neutral boundary that exists before any real
outbound provider is enabled.

## Current status

`Provider` and `ProviderTaskHandler` are application-level interfaces. No provider
handler is registered in production `main`, no real provider implementation exists,
and the deployed service still has no runtime network or secret access.

The current implementation is exercised only by deterministic offline tests.

## Request and result contract

A `ProviderRequest` carries:

- a deterministic idempotency key derived from provider identity and task identity;
- the task input content type and bounded input;
- the maximum output size;
- the task runtime budget.

A provider returns one of three outcomes:

- `succeeded`: a definite successful provider response;
- `rejected`: a definite provider response that means the task failed;
- `effect_unknown`: the caller cannot establish whether the external effect happened.

Provider exceptions are treated as `effect_unknown` by `ProviderTaskHandler`.

## Durable external-effect sequence

For a new provider task, `ProviderTaskHandler`:

1. checks cancellation before creating any external action;
2. creates a critical recoverable `Action` with deterministic identity;
3. starts the action with a bounded lease;
4. checks cancellation again while no external effect has occurred;
5. durably calls `record_effect_started()`;
6. only then invokes the provider;
7. records a definite response with `record_confirmed_result()`, or records ambiguity
   with `record_unknown_result()`.

The ordering of steps 5 and 6 is an invariant. The provider must never be invoked
before the durable unknown-effect marker exists.

## Replay policy

The first provider slice deliberately implements a stronger rule than the generic
wake runtime requires: **any pre-existing provider Action forbids automatic replay**.

This includes:

- an action already in manual review;
- a running action whose effect is unknown;
- an action that appears not to have crossed the effect boundary;
- a confirmed successful action whose provider response was lost before the Task
  result became durable.

The corresponding Task is moved to `manual_review`, and the provider is not invoked.
This intentionally trades some recoverability for a simple no-duplicate-provider-call
invariant.

A future adapter may relax this only when it can prove a retry is safe, for example
through a provider-enforced idempotency contract or a durable response receipt. Such a
relaxation must be explicit and tested; transport errors alone are not proof that no
external effect occurred.

## Cancellation

Cancellation is ordinary only before `record_effect_started()`. Once the provider
boundary is crossed, an interruption can be ambiguous and must not be represented as
if nothing happened. The current provider interface therefore has no post-invocation
`cancelled` outcome; ambiguous interruption maps to `effect_unknown` and manual
review.

## Tested cases

The offline fake-provider tests cover:

- successful response;
- definite rejection;
- ambiguous result;
- provider exception;
- cancellation before invocation;
- pre-existing uncertain action;
- confirmed call with a lost task response;
- replacement-process recovery after a crash following the durable effect marker.

Every uncertain or incomplete replay case asserts that the fake provider receives no
new call.
