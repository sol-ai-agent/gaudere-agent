# Local Goose cycle v2 — frozen bounded cognitive continuity

Status: **design-only**. No new Task kind, runtime, clock, sidecar, provider,
stimulus, B10 command, Fedora operation or production memory is enabled by this
document. A separately reviewed implementation and production gate are required.

## Motivation and boundary

The production Local Goose cycle v1 is based on the final generation-3 local
continuity observation and carries the **immediate** predecessor's canonical
decision in each new Task. The observed dormant cursor at generation 2 is the
*next opportunity*, following one completed generation-1 cycle; it is not proof
of two completed inferences. A dormant state is not a fault or a mandate to wake.

A later local cycle could benefit from remembering a few recent choices and
their rationale, instead of only one. This proposal concerns **internal
cognitive continuity**, derived solely from Gaudere's own already-durable
canonical cognition Tasks. It is **not** a collection, telemetry, observability,
statistics, search, analytics or reporting architecture. The general-purpose
information collection/statistical solution is being studied separately by Dev
and Bertrand. No new external information source is introduced here.

The existing `cognition.local-goose-cycle.v1` identity, Task inputs/results,
state cursor, historical observation pulse, WakeIntent and dialogue V1–V3
lineages remain immutable. The successful generation-1 result and the current
generation-2 dormant opportunity are not replayed or reinterpreted.

## Candidate V2 opportunity (not implemented)

Use a **distinct** future Task kind and input schema
`cognition.local-goose-cycle.v2` and
`gaudere.cognition.local-goose-cycle.v2`. The input binds the original
anchor/model/previous Task evidence, exact due/capture times and an embedded
**frozen memory window**. A V2 Task ID and idempotency key must cover the entire
canonical input, including the memory window. The V1 decision result contract
can be considered for reuse only after its exact compatibility is separately
proved. A V1 Task must never be silently promoted to V2.

The context window uses at most **four** immediate successful predecessor
cycle Tasks, ordered oldest-to-newest in the canonical input. Each entry
contains the exact Task ID, generation, SHA-256 of the exact canonical result
bytes, decision enum, scheduling intent, and a deterministic bounded
`assessment`/`reason` excerpt. No raw prompt, tool result, provider request
text, secret, filesystem path, profile or unrelated dialogue enters this
memory. `request_openai` remains a recorded enum only, never execution
authority. A snapshot may omit older generations by policy, but may not quietly
skip a missing or noncanonical Task **within** the chosen window.

Proposed caps, subject to implementation testing:

- four predecessor Tasks at most and their immediate links only;
- result bytes validated against the current 16-KiB decision contract and
  canonical JSON before hashing;
- UTF-8 safe excerpt at most 1,024 bytes per assessment/reason, with explicit
  truncation metadata and an exact source-result SHA-256;
- at most 12 KiB of canonical memory JSON; at most 48 KiB for the final Task
  input and existing runner prompt, including the fixed instruction prefix;
- fixed bounds, not caller-adjustable controls or an invitation to scan the
  TaskStore.

These limits bound working context; they do **not** cap the immutable stored
lineage's lifetime or claim an audit of the complete historical chain.

## Trust and deterministic reconstruction

The only initial root is the exact predecessor referenced by a canonical
cycle cursor. On the sole Agent owner thread, freeze the cursor once, validate
fixed scope, generation, anchor ID/result digest and pinned model digest, then
read only Task IDs derived from the canonical predecessor links. For each
included Task validate succeeded state, strict canonical result bytes,
deterministic Task identity, exact generation, anchor/model equality and the
link digest/embedded decision against the next record. No search, arbitrary
Task ID, unbounded traversal or second process opening production state.db.

A broken, absent, noncanonical, over-limit or ambiguous link fails **closed**;
it never falls back to a fresh empty memory, silently suppresses a Task or
submits a provider request. Earlier generations outside the bounded window are
explicitly reported as omitted, not claimed validated.

At opportunity preparation, compute and persist the V2 Task's frozen canonical
input **before** running Goose. A crash before Task submission must reconstruct
the same Task ID from the same durable facts and deterministic truncation.
If source evidence has changed, stop for review instead of minting an alternative
Task. Retry, lease recovery, terminal failure and generation advancement retain
the one-opportunity/one-Task discipline. No memory is appended after execution.

The memory excerpt is **untrusted model output** even when it passes canonical
structural validation. The V2 prompt must mark it as historical evidence, never
as governance instructions or tool permissions. Stored text must not override
provider OFF, network, shell, secrets, human approval or platform policies.

## Activation and separation

A future V2 runtime cannot simply use the existing V1 cursor as write authority:
the migration/bridge from the V1 dormant generation to any V2 opportunity,
choice of V2 sidecar or new cursor schema, and rollback proof require a separate
design. Creating this document or shipping a V2 library must **not** create a
new task, schedule a wake, consume a stimulus or reinterpret the V1 dormant
decision as `continue_local`.

An explicitly approved durable stimulus, if ever used, must continue through
the existing independently gated stimulus contract. No new implicit cadence
or automatic conversion from a dialogue, a GitHub event, a statistic, or a
provider-request record is authorized here.

## Delivery sequence

1. Review this design against immutable V1 production evidence and the existing
   dialogue V2 bounded-history implementation; settle exact field/cap details.
2. Implement a pure bounded lineage-to-memory builder with fake Task reader
   and adversarial tests, **unwired**, provider-free. It must not parse general
   database rows or expose a telemetry feed.
3. Implement V2 canonical Task identity/inspection and frozen context-to-prompt
   rendering with deterministic replay/crash fixtures; no runtime authority.
4. Only then propose a separate V1-to-V2 cursor/activation compatibility design,
   isolated Fedora proof and explicit production authorization. No B10/portal
   or information-collector integration in these stages.

Every stage requires code review and CI on an exact commit. Nothing in this
document authorizes Fedora deployment, a real wake, a real inference, OpenAI,
or modification of prior Tasks.
