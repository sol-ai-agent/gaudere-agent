# gaudere-chat /again — canonical answer replay in the current process

## User-facing behavior

- After a preferred-thread V3 Task has committed and its response passed the existing canonical validation, the local command `/again` reprints the **exact last verified response text**, marked as **cached**. No socket or Task inspection occurs.
- After an observation timeout, `/again` reports that no canonical response is available. The user can explicitly use `/wait` to continue observing the same verified Task without submitting it again.
- When `/wait` later validates that Task's response, `/again` can print it. `/last` still gives the verified committed Task ID independently of whether its response is ready.
- A **new verified commit** replaces the last committed Task and invalidates the prior cached response immediately; a pending/failed/noncanonical response of the new Task cannot return stale text. A conflict or ambiguous submit without a new proven commit cannot manufacture a new cached answer.
- `/again extra` is rejected locally; literal input `/again` as a dialogue text requires `//again`.
- Closing and restarting the process clears the cache entirely. This feature does not claim durable or cross-session recovery.

## Trust boundary

`GaudereChatSession::last_verified_response()` is backed by an optional field on `CommittedAttempt`. It is populated **only after** `canonical_response()` accepts the response for the original `request_id`, root and predecessor verified on the V3 preferred-thread commit. A status-only `/task ID` lookup cannot populate it, and `/again` never invokes transport, provider, ACK, retry, submit, filesystem or B10. This is not independent cryptographic attestation.

## Tests

Synthetic C++ tests prove the cache is absent before commit, after an ambiguous attempt, after a timeout and after a forged V3 response; populated after canonical success (including explicit `/wait`); and invalidated when a second verified commit remains pending. Parser tests cover exact `/again`, `//again`, malformed trailing arguments and unknown case. Fedora 45 CI runs the actual CLI with a nonexistent socket to prove the no-I/O local behavior.
