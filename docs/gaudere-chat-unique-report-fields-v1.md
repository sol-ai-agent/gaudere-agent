# gaudere-chat — unique live-control report keys (v1)

**Source-only.** No Fedora host deployment, B10, provider, production
Task/ACK or held responder sequence changes.

## Defect

The line-oriented LiveControl parser previously populated a
`std::map<std::string, std::string>` with `out[key] = value`.
This silently made a later duplicate line overwrite the first one.
The same parser is used for:

- preferred-thread head inspection (root/head/revision),
- durable V3 preferred submission confirmation (Task ID and revision),
- Task completion observation (status, result envelope),
- status-only `/task ID`, and conflict diagnostics.

`/task ID` already had a special-case guard against duplicate
`id`, `kind` and `status`, but other paths could still accept
contradictory key/value lines. In particular a duplicate `id` or
`revision` could undermine the claim that a Task was **verified as
durably committed**. A duplicate result field could affect response
verification. Silent last-wins is unsuitable at this trust boundary.

## Behavior

The shared parser now calls `map.emplace` and rejects **every**
duplicate key with a parse error, *including a duplicate with identical
value or an unknown diagnostic key*. The previous ad-hoc three-field
duplicate scan in `/task` is removed; it relies on the same shared
strict parser. All other semantics and transports are unchanged:

- A malformed **head** fails without a submit.
- A malformed **commit confirmation** fails closed without inventing a
  verified Task ID, revision or resubmission right.
- A malformed **post-commit Task observation** remains `invalid_reply`
  but preserves the previously validated Task identity and revision
  (#300); it cannot display a non-canonical V3 answer. The user can
  deliberately `/wait` in the same session, or `/task ID` for status
  without a second submission.
- Contradictory status-only reports are not accepted. Neither `/wait`
  nor `/task` performs a new submit, ACK, provider call or B10 action.

The existing format's optional unparsed human-readable header lines
remain allowed; only repeated `key=value` fields cause rejection.

## Regression

Three new table-driven suites inject duplicate head, commit, and Task
report fields, including a repeated *otherwise unknown* field and a
duplicate canonical output. Tests confirm that no precommit read creates
a Task, no duplicated confirmation creates a verified committed context,
and a postcommit invalid response preserves the real identity for an
explicit subsequent read-only recovery with exactly **one** submit.

Both the established Fedora 44 provider-free gate and a disposable
Fedora 45 build compile and run the updated `gaudere_chat_test` and
`live_control_test`. This does not test or change the user's Fedora 45
desktop, the Max-owned B10 gateway, or the production provider.
