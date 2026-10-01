# gaudere-chat v1 — local human dialogue UX

Status: design contract only.

This document starts after Stage 9J6 closed operationally with the first bounded
production responder turn. It does **not** authorize another production dialogue
turn, a responder ACK, a provider call, a cycle stimulus, or any deployment.

## Purpose

`gaudere-chat` provides Bertrand with a simple local conversational UX over the
already-proven preferred V3 dialogue thread.

The first version is a human chat client only. It is not a responder driver and
it does not consume the durable completion feed.

## Existing production primitives

V1 reuses the current Unix live-control protocol:

- `local-thread-v3-head THREAD_ALIAS`
- `local-thread-v3-send REQUEST_ID THREAD_ALIAS EXPECTED_REVISION human bertrand dialogue MESSAGE`
- `task TASK_ID`

No new network protocol is required.

The implementation should call `run_live_control_client()` directly. It must not
construct shell commands or exec `gaudere-control`.

## Default invocation

```
gaudere-chat
```

Defaults:

- socket: production local-control socket selected by deployment;
- thread alias: `main`;
- speaker kind: fixed `human`;
- speaker id: fixed `bertrand`;
- message kind: fixed `dialogue`.

Optional operator switches may expose the socket and thread alias. V1 must not
expose arbitrary `speaker_kind`, `speaker_id`, or `message_kind`.

## Interactive loop

For each user message:

1. inspect the preferred head for the configured thread;
2. retain its exact revision locally;
3. create one stable request id for this attempted message;
4. submit exactly one preferred V3 successor using CAS against that revision;
5. if submission is accepted or an exact duplicate, retain the returned Task id;
6. wait by bounded read-only Task inspection until terminal state;
7. require canonical successful V3 result;
8. print only the Gaudere response to normal chat output;
9. return to the prompt only after that turn is terminal.

An empty line may be ignored. EOF or an explicit local quit command exits without
submitting anything.

## Request identity

A request id belongs to one attempted message and must remain stable across
transport retries of that exact attempt.

A retry must never silently create a second semantic message.

V1 may use a bounded per-process session nonce plus a monotonic turn counter and
message digest. The implementation must use an OS randomness primitive rather
than time alone for the session nonce.

Request-id construction is local metadata only; canonical Task identity remains
defined by the existing V3 contract.

## Preferred-head conflict

A CAS conflict is not a transport retry.

If the preferred head changed between inspection and submission, `gaudere-chat`
must:

- report that the thread advanced concurrently;
- show the new head revision after a fresh read;
- keep the user's unsent message available for explicit retry;
- **not** automatically rebase and resubmit it.

This matters because a concurrent system responder or another human client may
have changed the conversational meaning of the message.

## Task waiting

Waiting is bounded observation, not scheduler authority.

The client may inspect the submitted Task periodically with a finite timeout.
It must not:

- stimulate the autonomous cycle;
- accept/revoke wakes;
- call a provider;
- create responder leases/intents;
- ACK any dialogue feed event.

Timeout leaves the durable Task untouched and tells the operator how to inspect
it later.

## Feed isolation

`gaudere-chat` v1 has **no completion consumer id**.

It never calls:

- `dialogue-feed-next`;
- `dialogue-feed-ack`;
- any `dialogue-responder-*` operation.

Therefore current production cursors remain independent:

- manual/system observation cursor `sol`;
- responder cursor `system-responder-v1`.

In particular, the currently pending responder completion sequence 3 is not part
of chat-client state and must not be ACKed by `gaudere-chat`.

## Output

Normal interactive output is intentionally small:

```
Bertrand> ...
Gaudere> ...
```

Operational failures go to stderr.

A verbose/debug flag may expose request id, Task id, thread revision and terminal
state, but canonical result JSON should not be dumped by default.

## Local commands

V1 may support only local non-model commands such as:

- `/quit`
- `/head`
- `/help`

These commands never become dialogue Tasks.

No shell escape, arbitrary control operation, file execution, or provider command
is part of `gaudere-chat`.

## Security boundary

The client is a local AF_UNIX operator UX.

It must not:

- open a TCP listener;
- expose the control socket to the model;
- read secrets;
- enable MCP/tools for Local Goose;
- add provider fallback;
- mutate responder state;
- mutate autonomous-cycle state.

The production service remains `Network=none`.

## Coexistence with the bounded responder

Human and system turns share the same preferred V3 head and therefore serialize
through the existing CAS.

A human turn may make an older pending responder observation stale. That is
expected: responder policy must revalidate its exact trigger/head and fail closed.
The chat client must not repair, consume, or reinterpret responder state.

## V1 non-goals

V1 does not include:

- multi-user identity selection;
- system-speaker chat;
- automatic responder continuation;
- conversation search;
- long-term memory UI;
- file/context attachments;
- batch mode;
- HTTP server/API.

Those remain separate later stages.

## Delivery stages

1. **Stage 9K1 — this design**: freeze UX and authority boundaries.
2. **Stage 9K2 — client implementation**: native C++ `gaudere-chat`, unit/fake
   socket tests, no production deployment.
3. **Stage 9K3 — isolated Fedora proof**: disposable/copied state, multi-turn
   human chat, restart/conflict/timeout proofs, provider-free.
4. **Stage 9K4 — production code deployment**: binary present, no chat turn sent.
5. **Stage 9K5 — first real human chat turn**: separate explicit human gate.

A later batch/API line can reuse the same typed client layer:

```
gaudere -f context -o output "prompt"
```

but must receive its own design for structured context, multiple `-f`, atomic
output, and optional JSON.

Every stage preserves:

`code -> CI green -> merge -> Fedora deployment/integration -> real Fedora proof`
