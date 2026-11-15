# gaudere-chat — cumulative Task observation deadline (v1)

**Source-only hardening. No Fedora deployment, provider, real Task,
ACK, held responder sequence, B10, or Podman action.**

## Problem

After a durable preferred V3 dialogue submission, `wait_for_task` repeatedly
uses `inspect_task` until a terminal Task state or `--timeout-ms`.
Previously the `chat_main` transport gave **the full `options.timeout`**
to *every* inspection, and a poll sleep could overshoot the remaining
interval. The `steady_clock` check occurred only after an inspection.
Even after #295 bound a single AF_UNIX transaction with a monotonic
deadline, a succession of transactions could exceed the overall Task
observation window.

## Contract

The transport callback now receives an explicit **per-call I/O budget**
as its second parameter:

- Preferred thread-head inspection and **submission** keep the existing
  independent `--io-timeout-ms` budget (default 5000 ms). A submission
  I/O failure remains **ambiguous**: the original request identity and
  expected revision are retained for explicit, exact-request `/retry`.
- **Only after a confirmed durable submission**, `wait_for_task` starts
  one `steady_clock` deadline of `--timeout-ms` (default 10 minutes).
  Before *each* Task inspection, it computes the remaining time. When
  less than one full millisecond remains, it returns a local timeout
  rather than starting an unbounded or zero-budget request.
- Each `inspect_task` passes this decreasing remaining budget to
  the socket client implemented in #295. An overdue return from a
  misbehaving transport is not accepted even if it claims success.
- A pending Task's next poll sleeps for at most the lesser of the
  configured poll interval and the remaining budget.
- On expiry or failed Task read, the Task is still **durably submitted**.
  A timeout does **not** produce an ambiguous submission, does not
  create a new Task, does not authorize an ACK and does not retry.
  The user may separately inspect its recorded Task ID.

The bound applies to **the post-commit Task observation loop**, not the
head/submit phase, user interaction, an external worker, or Task execution.
The clock is monotonic; no wall-clock timestamps are used to grant
execution or to change an authorization boundary.

## Regression

Synthetic time tests assert (1) budgets for a 12 ms total window
shrink exactly **12 ms → 7 ms → 2 ms** through three inspections,
with capped sleep and no fourth call, and (2) even a forged successful
response returned by a test transport after the deadline is rejected
as a timeout without a second submit. The existing conflict,
ambiguous-exact-retry, Task read I/O failure and canonical-response
tests continue to run under Fedora 44.

Original #231 contains older bounded-client ideas but is based on a
pre-`--io-timeout-ms` snapshot and must **not** be merged blindly.
#295 already solved the per-I/O time budget. This change covers only
the **cumulative** remaining-budget layer above it.
