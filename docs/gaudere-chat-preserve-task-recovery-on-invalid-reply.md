# gaudere-chat: preserve a proven Task identity after invalid observation

**Source-only. No host Fedora 45, B10, provider, Task/ACK or held-seq4 changes.**

## User-visible defect

After `gaudere-chat` successfully confirms a durable preferred V3
dialogue Task, `wait_for_task` inspects its completion. If the report
is malformed, claims a different Task ID, supplies an invalid result
envelope, contains a noncanonical V3 response, or carries an unknown
status, the historical `malformed()` helper loses the **known Task ID**
and committed revision. The CLI then prints just the parse error.

That creates an avoidable recovery problem: the message was actually
committed and could still finish, but the user does not see its exact
ID in the error, and might wrongly re-submit the original message.

## Correct semantics

The exact Task identity has already been established **before**
`wait_for_task` by validating the preferred commit report. For an
invalid read-only observation, the new `invalid_committed` path:

- Returns `GaudereChatTurnCode::invalid_reply` and the original error
  detail, **without accepting or displaying any invalid response**.
- Preserves `request_id`, the *already proven* `task_id`,
  `expected_revision`, and the committed `current_revision`.
- Never infers a Task ID or revision from the untrusted malformed
  observation. It uses only the validated submission result.
- Updates the CLI advice to print: durable Task ID, **do not resubmit**,
  `/wait` (same process, original request/predecessor proof), or
  `/task ID` for a status-only inspection, including in a restarted
  process.
- Does NOT change precommit errors: where the commit cannot be
  verified, no Task ID or committed revision is invented.

No new permission, provider call, Task creation, completion ACK,
resubmission, shell command, B10 operation or persistence is added.
`/wait` remains bounded by a cumulative monotonic deadline (#296).
`/task` remains a single read-only status request (#297).

## Negative regressions

Synthetic tests cover five post-commit invalid observations: malformed
quoted field, wrong Task ID, missing successful-result envelope,
invalid canonical V3 response, and unknown status. Each must return the
**actual** Task ID, committed revision and original request ID, empty
response, with exactly one submission. A deliberate later `/wait`
then accepts a canonical reply from the **same** Task without a second
submission.

A separate test supplies an invalid **commit report** and proves the
result contains neither Task ID nor a reconstructed current revision,
and the subsequent `/wait` fails closed locally.

Both Fedora 44 provider-free CI and disposable Fedora 45 chat CI run
the tests against the pinned Gaudere core. No CI execution accesses
the operator's Fedora desktop.
