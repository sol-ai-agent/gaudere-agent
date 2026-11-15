# gaudere-chat — exact local slash command routing (v1)

**Source-only.** The Fedora 45 desktop belongs to the operator; this
change only concerns GitHub source, tests and disposable Fedora 44/45 CI.

## User behavior

In `gaudere-chat`, a leading `/` denotes a **local CLI command**.
The only supported commands are `/help`, `/head`, `/task ID`,
`/wait`, `/retry` and `/quit`. Each spelling and its argument
count must match exactly. A typo such as `/wai`, `/retry now`,
`/quit now` or `/task` with two arguments is rejected **before
any socket, Task, or preferred-thread operation**. This prevents
accidental submission of misspelled local commands as dialogue turns.

To intentionally send text beginning with a literal slash, type
`//...`; the **first slash alone** is removed. For example:

```text
Bertrand> //help
```

sends the literal text `/help` as a **new** user dialogue message,
subject to all normal submission/ambiguity guards. It does not execute
the local `/help` command. `//` is therefore a deliberate message
containing a single `/`. A slash following any leading spaces is
normal message text, not a command (`"  /help"` is literal).

## Implementation

`src/GaudereChatInput.hpp` supplies a deterministic, side-effect-free
parser returning one of `empty/text/help/head/task/wait/retry/quit/
unknown_command`, and the optional payload (Task ID or dialogue
message). `src/chat_main.cpp` calls it exactly once per terminal
line, rejects the unknown category locally, and passes only the
`text` payload to `GaudereChatSession::send_turn`. The Task ID
continues to be checked by `GaudereChatSession::inspect_task` before
the read-only socket call.

No authority or state-machine change: an ambiguous submit remains
ambiguous and retains its exact-request `/retry` guard; the `/wait`
read-only observation remains bound to the last verified committed
Task; neither the parser nor local commands acknowledge completion
or authorize a provider call.

## Verification

The existing `gaudere_chat_test` now covers correct commands,
argument counts, accidental typos, unknown high-risk verbs, Task ID
syntax boundaries, deliberate `//` escapes and ordinary text.

The Fedora 45 CI also executes the **real CLI** against a nonexistent
socket with four malformed slash commands. All four must produce
`unknown local command`, with no attempted connection or Task
submission. Fedora 44 provider-free CI guards the parser's presence,
build inclusion and routing, alongside existing chat functional tests.

No Fedora host deployment, B10/Max operation, Podman cleanup, provider
call or production Task/ACK is performed by this source-only PR.
