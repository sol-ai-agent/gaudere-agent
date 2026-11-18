# gaudere-chat — independent per-poll I/O timeout

## Problem

`GaudereChatOptions::timeout` is the overall monotonic time budget for
waiting on a committed dialogue Task. `GaudereChatOptions::io_timeout`
is a separate, usually much shorter bound for each live-control socket
operation. Before this fix, `wait_for_task()` passed the entire
remaining overall timeout to every `inspect_task` request. With default
options, one silent peer could consume up to 10 minutes rather than the
configured 5-second per-I/O limit.

## Bound

For each post-commit read-only Task inspection:

```cpp
io_budget = std::min(options_.io_timeout, remaining_monotonic_budget);
```

Both options must be positive, checked at session construction.
The previously established overall monotonic deadline stays unchanged;
a final partial read receives only its remaining time budget. Head lookup,
preferred-submit and standalone `/task ID` still use the independent
`io_timeout` directly.

A socket timeout **after a canonical preferred commit** is not evidence
that the Task did not persist or has failed. It does not resubmit or
clear `last_committed_`. The user may explicitly use `/wait` for a new
bounded, read-only observation of the exact previously committed Task.
An ambiguous **submit** remains governed by the separate, exact-id
`/retry` mechanism of PRs #305–#306.

## Tests and limits

The provider-free test suite checks per-call budgets `4,4,2` ms for
an overall 12 ms deadline, plus a 3 ms I/O timeout under a 100 ms
wait deadline. A synthetic network timeout preserves the canonical
committed Task identity; a later `/wait` recovers a validated answer
without any second submit. Fedora45 gaudere-chat CI builds and executes
these tests. No Fedora workstation, Podman, B10 lane, real provider,
production Task or ACK is touched by this source-only change.

A fake transport cannot prove real kernel socket timing. That independent
transport deadline remains implemented by the live-control client.
