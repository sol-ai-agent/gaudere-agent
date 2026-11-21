# gaudere-chat: explicit conflict retry recovery

A definite preferred-thread conflict (submit exit code 4) is distinct from an ambiguous submission. The session retains the rejected text in memory, independently of the last verified commit. `/retry` after a definite conflict first consults the current head; a failed read keeps the rejected text and does not create a request. A later explicit `/retry` may submit against the fresh head with a new request ID. There is never an automatic retry.

Read-only `/wait`, `/head`, `/task`, and `/again` do not discard the conflict. If the new submission becomes ambiguous, the conflict slot is replaced by the exact ambiguous attempt: only the same identity can be retried until a definite outcome. A definite rejection of that exact attempt restores the conflict slot. A verified commit consumes the conflict slot. An explicit new message supersedes a definite conflict, but cannot supersede an ambiguous attempt. Nothing is persisted across process exit.

The source-only regression `conflict_retry_survives_observation_test` uses a fake transport: a previously committed turn, a rejected next message, successful read-only `/wait`, unavailable head, explicit retry at a fresh revision, ambiguous effect, and exact retry followed by definite conflict. Counts and request IDs are checked. No production Task, ACK, provider, B10 or Fedora effects.
