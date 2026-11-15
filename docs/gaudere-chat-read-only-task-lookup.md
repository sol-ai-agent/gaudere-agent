# gaudere-chat — recovery-safe read-only Task status lookup

**Status:** source-only feature, CI-covered Fedora 44 + Fedora 45. No host
deployment, agent run, provider activation, Task submission, dispatcher,
completion-feed acknowledgement, B10 or Podman effect.

## User-facing behavior

When a dialogue Task was durably submitted but `gaudere-chat` stopped
waiting after `--timeout-ms`, the CLI already reports its canonical Task
ID. A user can now inspect the recorded status **without resubmitting**,
even after exiting and reopening the chat:

```text
Bertrand> /task cognition.local-goose-dialogue.v3:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
task=cognition.local-goose-dialogue.v3:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd status=running
```

The command accepts exactly `/task ID`. A missing/malformed ID or a
mistyped `/task...` command is handled locally, never forwarded as a
new dialogue message. It accepts only an existing *syntactically canonical*
V3 dialogue Task ID (64 lowercase hex digits after the fixed prefix).
Syntactic validation **does not prove** that the Task exists or belongs to
a particular user.

The operation is **one** `LiveControlOperation::inspect_task` using
the existing `--io-timeout-ms` bound; it is not a polling loop and is
independent of the prior turn's `--timeout-ms`. It inspects the
returned `id`, `kind` and recognized `status`; it fails closed on
mismatching identity, unsupported kind/status, malformed report, I/O
failure or uncertain result.

Accepted status vocabulary: `pending`, `running`,
`cancel_requested`, `succeeded`, `failed`, `cancelled` and
`manual_review`. In particular, `manual_review` must not trigger
a retry. The command prints **status only**, never `result_output`:
`status=succeeded` is a *Task state report*, not proof of a
canonical V3 response, not permission to continue, and never an ACK.

## Security and recovery guarantees

The new API uses the existing bounded read-only control-client
transport. It cannot send `submit_local_goose_dialogue_v3_preferred_next`,
`acknowledge_local_goose_dialogue_completion`, provider requests,
external actions, or a B10 job. It does not clear a previously ambiguous
submission or create a new Task ID. If the user lost the ID entirely, this
feature cannot recover it from unrelated state, nor does it infer a
permission to resubmit.

This does not change the interpretation of `/retry`: a potentially
applied submission remains ambiguous unless the existing exact
request/revision rule and explicit user input are satisfied.
The read-only report is separate from chat's canonical-response
verifier, which requires the known original request and predecessor.

## Tests and platform transition

- Existing Fedora 44 provider-free CI continues to build and execute
  `gaudere_chat_test` (and `live_control_test`).
- A narrow Fedora 45 CI workflow builds the pinned Gaudere core,
  `gaudere-chat` and both tests in a disposable GitHub Actions container.
  It **does not** interact with the user's upgraded Fedora 45 host.
- Negative tests cover malformed/v2/uppercase/injected ID, incorrect
  returned identity and kind, unknown status, malformed text, I/O
  failure, and no duplicate submission when querying the same Task
  after the first observation timeout.

Future functionality (separate gated work): richer terminal result
inspection with the exact durable request/predecessor proof, without
turning a status-only response into authorization to ACK or dispatch.
