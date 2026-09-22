# Local Goose direct dialogue v2 — durable conversation lineage

Status: design contract only.

This document follows the successful production proof of direct Local Goose dialogue v1.
It does **not** authorize a provider call, a new production dialogue, a Local Goose
cycle stimulus, additional tool authority, or a production deployment.

## Proven v1 baseline

The v1 direct-dialogue path is now proven in production:

- fixed local Goose model;
- provider/OpenAI remains OFF;
- production container remains `Network=none`;
- dialogue runs with `tools_enabled=false`;
- no MCP control socket or governance path is exposed to the dialogue runner;
- one bounded human message creates one durable
  `cognition.local-goose-dialogue.v1` Task;
- the durable response is bounded and inspectable;
- the Local Goose autonomous cycle and dormant-stimulus ledger are unchanged.

V2 must preserve those properties.

## Goal

Add durable multi-turn conversation without concatenating an uncontrolled transcript.

Each turn is an independently bounded Task that cryptographically references the
immediately preceding successful turn. The resulting thread is a hash chain of
canonical requests and canonical responses.

The chain is durable evidence. Runtime context expansion is separately bounded.

## New Task kind

Use a distinct Task kind:

`cognition.local-goose-dialogue.v2`

Input content type:

`application/vnd.gaudere.local-goose-dialogue-v2+json`

Result content type:

`application/vnd.gaudere.local-goose-dialogue-v2-response+json`

V1 Tasks remain immutable historical evidence and are never rewritten as V2.

## Root turn

A root V2 turn contains exactly:

- `schema = gaudere.cognition.local-goose-dialogue.v2`;
- bounded `request_id`;
- bounded human `message`;
- fixed production `model_sha256`;
- `turn_index = 0`;
- `root_task_id = ""`;
- `predecessor_task_id = ""`;
- `predecessor_result_sha256 = ""`.

The caller supplies only `request_id` and `message`.

After the Task identity is derived, that Task ID is the durable root identity for
the conversation. Successor turns carry that root Task ID explicitly.

The existing v1 production proof is not retrofitted into a V2 thread. V2 starts
with a fresh root turn.

## Successor turn

A successor V2 turn contains exactly:

- the same fixed V2 schema;
- a fresh bounded `request_id`;
- the new bounded human `message`;
- the fixed production `model_sha256`;
- `turn_index = predecessor.turn_index + 1`;
- the immutable `root_task_id`;
- `predecessor_task_id`;
- SHA-256 of the predecessor Task's exact canonical result bytes as
  `predecessor_result_sha256`.

The caller does **not** supply `turn_index`, root identity, model hash, or result
hash. Gaudere derives them from the durable predecessor Task.

A successor may be created only from one canonical successful V2 predecessor.

## LiveControl surface

Keep root and successor creation explicit:

`gaudere-control --socket PATH local-thread-start REQUEST_ID MESSAGE`

`gaudere-control --socket PATH local-thread-next REQUEST_ID PREDECESSOR_TASK_ID MESSAGE`

For `local-thread-next`, the main worker:

1. loads the predecessor from the durable Task store;
2. requires canonical V2 success;
3. hashes the exact canonical predecessor result bytes;
4. derives root identity and next turn index;
5. builds and submits the deterministic successor Task.

The socket thread receives no TaskStore or model authority.

The existing `local-message` v1 operation remains available and unchanged.

## Request identity

Request IDs retain the existing safe grammar and 1..128-byte bound.

A request ID is single-use across V2 dialogue requests:

- same request ID + exact same canonical definition => duplicate/retry;
- same request ID + different message or lineage => conflict;
- a new human turn requires a fresh request ID.

Task identity is deterministic over the exact canonical V2 input.

## Lineage validation

For every successor, canonical inspection must prove:

- Task ID matches canonical input;
- model hash is the configured fixed model;
- predecessor Task ID is a canonical V2 Task ID;
- predecessor exists durably;
- predecessor succeeded canonically;
- predecessor result hash matches the stored exact result bytes;
- predecessor model hash matches;
- predecessor root identity matches;
- turn index advances by exactly one;
- no self-reference;
- no cycle in a traversed chain;
- root turn has no predecessor fields;
- non-root turn has all predecessor fields.

A lineage mismatch fails closed. It never falls back to provider execution or a
fresh unlinked turn.

## Bounded conversation context

The durable lineage may grow, but the model input must remain bounded.

For one V2 inference, the handler deterministically expands at most:

- the current human message;
- the six most recent canonical predecessor turns;
- at most 40 KiB total UTF-8 history material before the fixed dialogue prompt;
- at most 48 KiB total runner prompt, preserving the existing LocalGooseRunner bound.

Each included predecessor contributes only its canonical human message and
canonical assistant response, in oldest-to-newest order.

If more than six predecessors exist, older turns remain durable in the lineage but
are omitted from model context. The prompt states that earlier durable history
exists but was not included in this bounded context window.

If the six-turn window itself would exceed the 40 KiB history bound, oldest
included predecessors are deterministically omitted until the bound is satisfied.

Lineage validation traverses at most 4096 predecessor links for one Task. A deeper
V2 thread fails closed rather than allowing unbounded validation work.

No raw database rows, metadata, hidden prompts, tool outputs, filesystem content,
or autonomous-cycle content is added to dialogue context.

## Local inference boundary

V2 preserves the v1 local inference boundary:

- fixed local model;
- `tools_enabled=false`;
- empty control socket;
- empty governance path;
- fixed Goose binary;
- `--provider local`;
- `--no-profile`;
- `--no-session`;
- no provider fallback;
- no network authority;
- no shell authority;
- no secret authority.

The human conversation remains data, not an authority grant.

## Durable response

A V2 result contains exactly:

- `schema = gaudere.cognition.local-goose-dialogue-v2-response.v1`;
- `request_id`;
- `root_task_id`;
- `turn_index`;
- `predecessor_task_id`;
- `predecessor_result_sha256`;
- bounded `response`;
- fixed `model_sha256`.

For a root result, `root_task_id` is the current Task ID and predecessor fields
are empty.

For a successor result, the lineage fields exactly repeat the canonical input
lineage evidence.

Gaudere owns canonical serialization.

## Concurrency and branching

V2 permits explicit branching from an older successful predecessor.

Two fresh request IDs may therefore create two different valid successor Tasks
from the same predecessor. They are separate branches with the same root.

V2 does not maintain a mutable "current thread head" pointer. This avoids races,
implicit ordering, and hidden mutation.

A later UX layer may present a preferred head, but that must not change the
underlying append-only lineage contract.

## Scheduler and autonomous-cycle separation

V2 dialogue does **not**:

- create or consume a dormant-cycle stimulus;
- mutate the Local Goose cycle cursor;
- schedule an autonomous wake;
- advance the autonomous generation;
- transform a conversation response into autonomous continuation;
- replay a historical autonomous Task.

Direct dialogue and autonomous cognition remain separate durable lineages.

## Recovery

Normal Task durability applies.

A committed V2 Task is recoverable through the ordinary Task runtime.

On restart:

- pending/running dialogue recovery follows normal Task lease semantics;
- lineage is revalidated before local inference;
- a broken predecessor reference fails durably;
- no provider fallback occurs;
- no stimulus is generated;
- no automatic successor turn is created.

## Staged delivery

1. **V2 design contract** — this document only.
2. **V2 canonical Task/result contract** — schemas, deterministic identity,
   predecessor validation primitives, tests; no production wiring.
3. **Bounded lineage resolver + tool-free handler** — fake-runner tests,
   six-turn / 48-KiB deterministic context window.
4. **LiveControl root/successor submission** — `local-thread-start` and
   `local-thread-next`, durable conflict semantics.
5. **Isolated Fedora proof** — real local model, at least three linked turns,
   deliberate branch, `Network=none`, provider unchanged, autonomous cycle and
   stimulus unchanged.
6. **Production code deployment gate** — capability wired but no V2 production
   thread started automatically.
7. **First explicit V2 production thread** — separate human authorization.

Every stage preserves:

`code -> CI green -> merge -> Fedora integration/proof -> real Fedora proof`

No merge or deployment authorizes a provider call, an autonomous-cycle stimulus,
or a production conversation turn.
