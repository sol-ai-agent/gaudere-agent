# Service readiness

`gaudere-agent: running` is a readiness statement, not merely a process-start marker.

For ordinary service mode, the message is emitted only after all mandatory resources for the selected mode have initialized successfully. In particular, when live control is configured, the Unix-domain control socket must already be bound and listening before `running` is written.

Standard output is configured as unbuffered operator output so readiness and shutdown transitions are visible to journald while the process is alive rather than only when the process exits.

Regression coverage in `tests/smoke.sh` checks both sides of the contract:

- an invalid mandatory control-socket path makes startup fail without ever emitting `gaudere-agent: running`;
- a valid live-control service exposes `gaudere-agent: running` in its redirected log before the process is stopped.

This rule was added after the first real Quadlet live-control deployment attempted `/run/gaudere-control.sock`. The process correctly failed the socket bind because UID 1000 could not write directly to `/run`, but the old ordering had already printed `running`, and stdout buffering delayed that line until process exit. The production socket path was separately corrected back to the real-host-proven `/tmp/gaudere-control.sock`.
