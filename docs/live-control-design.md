# Live control boundary (design note)

Gaudere's durable SQLite state has a strict one-process-owner invariant. A running
service owns `state.db`; therefore operator commands must not open the database from a
second process merely to submit, inspect, or cancel work.

The planned live-control boundary is a local Unix-domain socket owned by the running
service. The socket is not a TCP listener and is never published as an inbound network
port. Requests are bounded and queued in memory by a control thread. The control thread
must never mutate Runtime or SQLite. It only queues a command and wakes the existing
scheduler. The main worker drains queued commands and performs all durable Runtime/
SQLite transitions, preserving the existing single-worker invariant.

The first slice will support local operator submission/inspection/cancellation only.
OpenAI networking and the real provider secret remain separate deployment concerns.
