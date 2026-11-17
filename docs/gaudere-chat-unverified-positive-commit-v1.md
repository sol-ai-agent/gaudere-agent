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

## Regression evidence

The synthetic transport tests cover a positive report with only `status`,
a duplicate key in the success report, mismatched revision, alias, root and
head, blocking a second new turn without network use, refusal to promote an
unverified response to a Task identity, and explicit recovery using the exact
original request ID and fields. No provider, production Task, ACK, B10,
Fedora host or socket is used by the tests.
