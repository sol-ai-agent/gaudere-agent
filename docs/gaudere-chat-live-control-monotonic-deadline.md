# gaudere-chat: one monotonic LiveControl I/O deadline

**Source-only hardening. No runtime deployment, provider, Task/ACK or B10 action.**

## Problem

The current `--io-timeout-ms` client uses `SO_SNDTIMEO` and
`SO_RCVTIMEO` to limit individual socket system calls. Each completed
`recv(2)` renews that budget, so a peer that provides one byte every
20 ms can keep a 90 ms call alive far beyond 90 ms. A blocking `connect(2)`
is also not bounded by the socket receive timeout. With a sole Goose
worker, `gaudere-chat` may otherwise wait inside Task inspection
longer than the user expects.

## Change

The existing `run_live_control_client` API and `--io-timeout-ms`
option are preserved. When `io_timeout > 0`, the client opens a
nonblocking AF_UNIX socket and applies **one `steady_clock` deadline**
to the entire connect, send and receive sequence. `poll(2)` uses the
remaining budget. Every send and receive iteration independently checks
the deadline even if the socket never blocks. A breach returns the
existing transport error code 1, with an explicit `timed out`
diagnostic. The old `SO_SNDTIMEO`/`SO_RCVTIMEO` protection is retained
as defense in depth. A zero timeout keeps the historical unbounded
`gaudere-control` behavior.

A timeout on a potentially modifying submission is **not** evidence
that its effect was absent: `GaudereChatSession` retains the original
ambiguous request and requires explicit exact-request `/retry`.
A timeout while inspecting an already durable Task remains terminal
to the local chat wait and does not authorize a second submission.
No retry, provider or dispatch policy changes are introduced.

## Regression

`test_client_slow_trickle_does_not_extend_deadline` starts a
separate local AF_UNIX peer, reads a request and trickles response bytes
at 20 ms intervals. An overall 90 ms deadline must fail before 700 ms
instead of being repeatedly refreshed. Existing timeout and
protocol round-trip tests remain in the main Fedora 44 build; the
provider-free workflow asserts the new helper and regression test.

## Remaining boundary

This change limits **a single socket round trip**, not the full
end-to-end duration of `GaudereChatSession::wait_for_task` across
multiple inspections and sleeps. The latter still uses the existing
`--timeout-ms` policy and merits its own remaining-budget regression
test. It does not change the sole-worker architecture or enable a
real Fedora experiment. The original PR #231 proposed a similar
monotonic client but targeted the pre-`--io-timeout-ms` API; porting
its files verbatim would regress the current option and its tests.
