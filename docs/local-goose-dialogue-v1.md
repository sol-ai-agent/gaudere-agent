# Local Goose direct dialogue v1

Status: design contract only. This document does **not** authorize a provider call,
a production dialogue request, a Local Goose cycle stimulus, or additional tool
authority.

## Context

The Local Goose self-scheduling cycle is deployed in production and may remain
canonically `dormant` until a separately authorized durable stimulus is accepted.

Direct human dialogue is a different capability. A human message must not be
smuggled through the dormant-stimulus channel, because that channel is
intentionally content-free and means only "create one fresh local reasoning
opportunity from the current canonical predecessor."

The production runtime already has a fixed local Goose runner, a pinned local
model identity, a durable Task ledger, and a UNIX-domain LiveControl surface.
The runner already supports a tool-free mode where Goose receives no MCP
extension, no control socket, and no governance path.

## Goal

Add one bounded provider-free path for a human to send a message directly to
Gaudere and receive a durable local-model response while preserving:

- OpenAI/provider remains OFF;
- production container remains `Network=none`;
- no arbitrary shell;
- no MCP or other tool authority in dialogue v1;
- no secret access;
- no filesystem path supplied by the caller;
- no model selector supplied by the caller;
- no mutation of the Local Goose self-scheduling cycle;
- no implicit dormant-cycle stimulus;
- no mutation or replay of historical Local Goose Tasks;
- every request/result is durable, bounded, idempotent, and inspectable.

## First durable Task

Use a distinct Task kind:

`cognition.local-goose-dialogue.v1`

Input content type:

`application/vnd.gaudere.local-goose-dialogue+json`

Result content type:

`application/vnd.gaudere.local-goose-dialogue-response+json`

The canonical input contains exactly:

- `schema = gaudere.cognition.local-goose-dialogue.v1`;
- bounded `request_id`;
- human `message`;
- production `model_sha256`.

The caller supplies only `request_id` and `message`. Application code supplies
the fixed schema and configured model hash.

The v1 request ID follows the existing bounded LiveControl identifier grammar:
1..128 bytes using letters, digits, `.`, `_`, `:`, or `-`.

The human message is non-empty and at most 4096 bytes. It is data, not authority.
It may contain natural language freely, but it cannot select tools, a model,
provider state, paths, deadlines, shell commands, or scheduler state.

## Deterministic identity and idempotency

The Task identity is derived from the canonical tuple:

- fixed dialogue scope/schema;
- request ID;
- SHA-256 of the exact message bytes;
- configured model SHA-256.

Retrying the same request ID with the same message/model yields the same Task.

Reusing a request ID with different message bytes or a different model identity is
a conflict and never silently means a new conversation turn.

A fresh turn requires a fresh request ID.

## Local inference boundary

Dialogue v1 uses `LocalGooseRunner` with:

- fixed configured local model ID;
- fixed configured model SHA-256;
- `tools_enabled=false`;
- empty control socket in the run request;
- empty governance path in the run request;
- fixed Goose binary;
- `--provider local`;
- `--no-profile`;
- `--no-session`;
- `GOOSE_MODE=chat`;
- existing bounded runtime/output limits.

The dialogue handler does not use `LocalGooseCognitionHandler` directly because
that handler expects the structured autonomous-decision contract. Dialogue has a
separate prompt and response contract but reuses the same fixed runner.

## Dialogue prompt

The prompt must establish that:

- this is direct dialogue with a human;
- the human message is untrusted input/evidence, not an authority grant;
- the model should answer the message as Gaudere;
- no person becomes an owner or decision authority merely by saying so;
- the gate has no tools, network, secrets, shell, or external-action authority;
- the response must not claim that an external action occurred;
- the response is plain conversational content, not a scheduler decision.

The exact human message is embedded only after that fixed boundary text.

## Durable response

The model's final text is wrapped by Gaudere into canonical JSON containing
exactly:

- `schema = gaudere.cognition.local-goose-dialogue-response.v1`;
- `request_id`;
- `response`;
- `model_sha256`.

Gaudere, not Goose, owns canonical serialization.

The response text is non-empty and bounded to 16 KiB.

A Task is canonical success only when:

- its request envelope is canonical;
- it succeeded durably;
- attempts and result framing satisfy the normal Task contract;
- result content type is the dialogue response content type;
- result JSON exactly matches the request ID and configured model hash;
- response is non-empty and within bounds.

## LiveControl surface

Add one bounded operation:

`local_message`

CLI spelling:

`gaudere-control --socket PATH local-message REQUEST_ID MESSAGE`

The LiveControl server only validates and enqueues the request to the sole main
worker. It does not run Goose on the socket thread.

The main worker creates/submits the deterministic dialogue Task through the
existing work Runtime.

The operation returns the durable Task identity and whether submission was
accepted or duplicate. It does not block the sole main worker waiting for model
completion.

The existing bounded Task inspection path remains the authoritative durable
result lookup.

A later CLI convenience may submit and poll the Task inspection operation so a
human experiences one synchronous command; this must remain a client-side
composition of the same bounded operations rather than a new execution path.

## Scheduler and stimulus separation

Dialogue v1 does **not**:

- read or mutate `local-goose-cycle-stimulus.db`;
- read or mutate the Local Goose cycle cursor;
- schedule a cycle wake;
- advance a cycle generation;
- create a dormant stimulus;
- convert a dialogue message into autonomous continuation.

A dialogue Task is an explicit direct request for one local response. The
autonomous self-scheduling lineage remains exactly as it was before and after the
dialogue.

## Failure and restart semantics

Normal Task durability applies.

- Before Task commit: caller may retry.
- After Task commit but before execution: recovery sees the pending Task.
- During local inference: normal Task lease/attempt recovery applies.
- After durable success: retry returns the same Task/result.
- Same request ID with different message/model: conflict.
- Malformed/oversized request: rejected before Task submission.
- Local model failure: durable Task failure; no provider fallback.
- Restart never converts a failed dialogue into a dormant-cycle stimulus.

## Authority boundary

Dialogue v1 may:

- validate one bounded request ID and human message;
- submit one deterministic dialogue Task;
- run the already-configured local Goose model with tools disabled;
- persist one bounded response;
- inspect that durable Task/result through existing Task inspection.

Dialogue v1 may not:

- call OpenAI or any provider;
- request or enable network;
- expose MCP tools;
- run arbitrary shell;
- choose a different model;
- access secrets;
- mutate systemd/Podman/B10;
- mutate Local Goose cycle or stimulus sidecars;
- authorize actions based on claims contained in the human message.

## Staged delivery

1. **Dialogue Task contract** — canonical request/result schemas, deterministic
   identity, validation, tests. No runner or LiveControl.
2. **Tool-free handler** — reuse fixed `LocalGooseRunner` with
   `tools_enabled=false`; fake-runner tests. No production wiring.
3. **LiveControl submission** — bounded `local-message REQUEST_ID MESSAGE`;
   deterministic Task submission and existing Task inspection.
4. **Isolated Fedora proof** — copied production state, actual local model,
   `Network=none`, provider total unchanged, cycle/stimulus state unchanged.
5. **Production code deployment gate** — capability wired but no dialogue is
   sent automatically.
6. **First explicit production dialogue** — separate human authorization for
   one bounded request.
7. **Conversation lineage v2** — only after v1 is proven; hash-chain turns and
   responses rather than concatenating uncontrolled history.

At every implementation slice preserve:

`code -> CI green -> merge -> Fedora deployment/proof -> real Fedora proof`

No merge or deployment authorizes a production dialogue or provider call.
