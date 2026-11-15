# gaudere-chat: resume last committed Task observation, read-only

**Source-only feature. No Fedora host, B10, provider, production Task, ACK or
responder sequence modification.** Validated by disposable Fedora CI only.

## User-facing behavior

After the original `gaudere-chat` turn was **durably committed** but its
response did not become terminal before `--timeout-ms`, the terminal already
shows its exact Task ID and the read-only status command `/task ID`.
The new command `/wait` lets the user **wait again for the last verified
committed turn in the same running chat session**, using a fresh bounded
`--timeout-ms` observation window:

```text
gaudere-chat: Task cognition.local-goose-dialogue.v3:... is durably
submitted but could not be observed as terminal within the bounded wait.
Do not resubmit this message. Use /wait to await the last verified
commit again in this session, or /task <ID> to inspect its status read-only.
Bertrand> /wait
Gaudere> Bonjour Bertrand.
```

`/wait` performs **inspection only**: no new submit, no API/provider call,
no Task dispatch, no ACK, no automatic retry and no alteration to an
ambiguous submission. It uses the **existing** `wait_for_task` implementation
and its cumulative monotonic timeout introduced in PR #296; every socket
exchange is independently bounded by PR #295.

## Security and consistency

- A `CommittedAttempt` (exact original request ID, original predecessor
  `GaudereChatHead` and exact Task ID) is retained in memory **only
  after validating** the durable preferred-thread commit result's ID,
  kind, alias, root, head ID and incremented revision.
- A `status=succeeded` field or `/task ID` lookup **cannot create**
  `CommittedAttempt` and is not proof of a canonical dialogue response.
- `/wait` reuses the original `canonical_response` verifier, checking
  the request ID, root, predecessor ID, response schema and speaker
  constraints. Terminal failure/invalid reply remain failures.
- An ambiguous submit cannot be promoted to a verified commit. An
  earlier confirmed Task may remain available for `/wait` even if a
  different later submission becomes ambiguous; `/wait` does not
  resolve that ambiguous state.
- If the chat process exits, the in-memory attempt is lost. `/wait`
  deliberately does not reconstruct authority from an arbitrary Task
  ID or unverifiable socket report. On a restarted process, `/task
  ID` remains available for status-only lookup.
- When another verified preferred V3 Task is committed, `/wait` refers
  to the **most recent** verified one. It is not a global history index
  and never offers automatic replay of earlier turns.

## Regression cases

1. Synthetic fake-clock Task pending, original wait times out at 3 ms;
   explicit `wait_last_committed()` then reads the same durable ID and
   returns the canonical response. Only ONE `submit` occurs.
2. No verified commit: fail locally before transport.
3. Ambiguous submit: no `/wait` shortcut, original ambiguous attempt
   still preserved for the explicit exact-request `/retry` rule.
4. Forged terminal response (wrong `speaker_kind=system`) is rejected
   even when returned for the previously committed Task; never shown
   as a canonical chat answer.
5. Existing `/task`, bounded I/O, conflict and negative-report tests
   continue to run on both Fedora 44 and Fedora 45 CI.

The result is an ergonomic recovery of a known committed Task, not a
new cryptographic trust boundary or production authorization.
