# gaudere-chat: fail closed after an unverifiable positive V3 commit report

## Motivation

A `submit_local_goose_dialogue_v3_preferred_next` call may have durably
committed the preferred-thread Task *before* the client receives its success
report. A transport return code of zero is **not** evidence of a validated
commit if the returned report is malformed, duplicated or inconsistent with
the original request, root, alias, predecessor or expected revision.

Before this change, the client returned a generic invalid reply and cleared
the ambiguous attempt. The user could then send a different turn without
first resolving a previous potentially committed submission.

## Safety contract

- A malformed, duplicate-key or non-canonical **successful submit report**
  preserves the entire original attempt in process memory: request ID,
  message, thread alias and exact expected predecessor revision/root.
- Return `transport_ambiguous` with a precise explanation that a durable
  commit may exist; do **not** claim a Task ID, canonical answer or new
  preferred revision without a verified commit report.
- Do **not** inspect a Task based on an unverified ID, send a different turn,
  generate a new request ID or automatically retry.
- `/retry` remains an explicit user action; it reuses the exact idempotency
  key and original request payload. A verified commit then clears ambiguity.
  If a server instead reports a definite conflict, the existing conflict
  handling applies and still does not invent a verified commit.
- `/wait` remains limited to a previously verified preferred-thread commit;
  a malformed positive submit report by itself never authorizes `/wait`.
- The successfully validated commit path and the no-socket local commands
  `/last` and `/again` remain unchanged. The previous successfully
  verified Task identity is not erased by an unrelated ambiguous attempt.

This is an *in-session* preservation mechanism, not a durable journal
across process restart and not proof that the uncertain Task was committed.

## Strict V3 commit identity

A preferred V3 commit report must carry a **V3-formatted** Task ID
(`cognition.local-goose-dialogue.v3:` + 64 lowercase hex digits), a
matching `head_task_id`, the expected V3 Task kind, and a present, known
Task status. A syntactically valid **V2** Task ID is valid as a possible
historical root/predecessor, but never proves a newly committed V3 Task.
Likewise a truncated report without `status`, or one with an unknown
status, cannot grant `/wait` or `/last` authority. Such a positive
report keeps the same ambiguous request for explicit `/retry`.
A code-4 conflict can only advertise a possible V3 orphan ID if its
format is actually V3; it never promotes an unrelated V2 ID to a V3
orphan identity.

## Successor-only result envelope

For `submit_local_goose_dialogue_v3_preferred_next`, a successful V3
reply must describe a **successor turn**. `turn_index=0` is reserved for
root dialogue Tasks and cannot be replayed as a successor answer, even if
the JSON bytes are canonical and request/root/predecessor fields match.
The session rejects a root-only response without clearing the already
verified durable Task identity or populating the `/again` cache. A later
explicit, read-only `/wait` may recover the correctly validated V3 reply
without submitting another Task. This protects the client against a bogus
success report; it does not independently prove the model SHA, predecessor
result hash or the exact positive turn index without the canonical Task
input (which the chat client does not retrieve).

## Non-success transport replies

Only explicit server rejection `code=4` means the preferred-thread
commit did not succeed (although a separate orphan Task may still exist).
`code=1` is a transport failure and `code=2` is a protocol error:
a response could have been too large **after** the server had persisted
the Task. Unknown nonzero codes are also not a reliable no-side-effect
proof. All nonzero codes other than 4 now retain the original request
and demand an explicit, same-identity `/retry` before new text.
Repeated errors retain the same request rather than incrementing its
turn counter. This in-memory protection does not survive process exit.

## Exceptions from the submit transport

An injected transport or a future socket-adapter layer may **throw** rather
than return a numeric status, potentially after sending the V3 request or
receiving a server-side durable commit. Both standard C++ exceptions and
unknown exception types therefore produce `transport_ambiguous`, not an
uncaught termination. The session retains the exact request ID, text,
preferred-thread alias, root and predecessor revision in process memory.
A new message is blocked, `/wait` cannot invent a committed Task identity,
and only explicit `/retry` can re-issue the same idempotent request.
Repeated throws cannot allocate a fresh request ID. This protection is
in-memory only: it does **not** constitute a durable crash/restart journal.

## Regression evidence

The synthetic transport tests cover a positive report with only `status`,
a duplicate key in the success report, mismatched revision, alias, root and
head, blocking a second new turn without network use, refusal to promote an
unverified response to a Task identity, and explicit recovery using the exact
original request ID and fields. No provider, production Task, ACK, B10,
Fedora host or socket is used by the tests.
