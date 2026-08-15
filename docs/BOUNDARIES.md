# Repository boundaries

## gaudere

Public generic C++ library. It owns reusable scheduling, recovery, and
persistence components. It contains no agent identity, provider integration,
Second Life code, or host policy.

## gaudere-agent

Public application runtime. It owns orchestration, provider adapters,
configuration contracts, local commands, and deployment assets.

## gaudere-continuity

Private structured continuity record. It owns identity framing, principles,
compressed history, architectural decisions, current state, and handovers.
It contains no executable secrets.

Runtime state such as SQLite databases remains outside Git in persistent local
storage.
