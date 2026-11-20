# gaudere-chat: read-only transport throws (provider-free)

## Observable failure

A LiveControl transport callback is a `std::function` and may throw while
inspecting a preferred thread head or Task. The interactive `chat_main.cpp`
outer exception handler then exits, losing the in-memory `/wait` context
after a verified V3 commit. Socket adapters usually return an error code,
but the callback contract does not guarantee that.

## Source-only behavior

- `inspect_head`: catch standard and unknown transport exceptions. Return a
  failed inspection; no request ID or submit is created.
- `/task ID`: catch exceptions from its single read-only inspection. Return a
  failed status-only read; never infer a canonical response or grant ACK.
- Post-commit observation and `/wait`: catch read-only inspection exceptions
  and report an invalid observation **with the already verified Task ID and
  revision**. Keep the original request ID, predecessor, and in-memory commit
  for explicit `/wait`; do not resubmit, retry implicitly, ACK, or call the
  provider. Do not copy exception text into the CLI.
- Exceptions on the **side-effecting submit** still use the distinct exact
  idempotent `/retry` ambiguity path. A status lookup cannot establish a
  preferred commit, and `/wait` cannot survive process exit without an
  separately authorized persistence design.

## Evidence and limits

`thrown_read_only_transport_preserves_session_test` injects both
`std::runtime_error` and an unknown exception type into head and Task
read-only callbacks. It proves no premature submit, identity preserved after
committed Task inspection failure, status-only failure without response cache,
and successful `/wait` of the **same** Task with exactly one submit.

The test is synthetic and does not validate the Fedora host or deployment.
No host action, B10, provider, production Task, ACK, SQLite state, or operator
gate is exercised by this change.
