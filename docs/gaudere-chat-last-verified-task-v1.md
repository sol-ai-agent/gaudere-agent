# gaudere-chat: /last — last locally verified committed V3 Task

## User behavior

- `/last` displays the ID of the last Task whose V3 preferred-thread **commit response** this running chat session actually verified. It uses only local memory and performs **zero LiveControl/socket calls**.
- `/wait` continues bounded, read-only observation of that Task with the original request ID and predecessor context; `/task ID` provides read-only status (not a verified response).
- Before any proven commit or after process restart, `/last` reports that there is no verified committed Task. It does **not** query or reconstruct a commit from an arbitrary ID or a `status=succeeded` report.
- If a later head read or Task observation fails, `/last` still shows the earlier proven Task. A failed, conflicting or ambiguous submission cannot create a new proof from its status alone.
- Literal text `/last` must be sent as `//last`; `/last extra` is rejected locally rather than submitted as a Task.

## Trust boundaries and limitations

The local accessor `GaudereChatSession::last_verified_committed_task_id()` exposes only `CommittedAttempt::task_id` captured *after* validation of the actual preferred-thread commit (ID, kind, alias, root, new head and strictly incremented revision). It does not constitute independent cryptographic attestation, cross-process persistence or provider authorization. In-memory state is lost on exit.

No Task submission, retry, ACK, provider call, B10 request, filesystem write or Fedora host operation occurs when `/last` is invoked.

## Verification

`gaudere_chat_test` checks recognized/escaped/rejected syntax, presence of a verified ID after a canonical committed turn, absence for an ambiguous/uncommitted turn, and that a status-only `/task` lookup never creates the commit context. Fedora 45 CI additionally runs the real CLI with a deliberately nonexistent AF_UNIX socket and confirms that `/last` requires no connection.
