# PREP ONLY — rollback-safe production promotion for c8b0999

This document records the bounded objective chosen by real current-cognition call #7. It is a **plan, not deploy authority**. Reading, merging, or satisfying this document does not authorize a production image change, WakeIntent activation, provider call, successor action, shell action, or tool use.

## Frozen evidence

The promotion target is immutable:

- candidate tag used only for operator readability: `localhost/gaudere-agent:current-cognition-c8b0999cf893`;
- candidate image ID: `sha256:9dd20dbacee908e3a760080c4495a38991c208ffc81e90e48066156ec46072a9`;
- Agent revision: `c8b0999cf8935ae60921b4031a0db927e3901c23`;
- Core revision: `1316cf68db93e4c91a7bd79fbd289b8f382f8659`.

The production baseline before promotion is:

- current production image: `sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01`;
- secondary historical rollback image: `sha256:3102c736e9365c81ae1090e26b6aa2c94b4562fe860cca4d96c57f23313630a3`;
- older historical rollback image: `sha256:6f2dab2ece7783556647f99204e4620a53b6574319310f5c61ffad8b579773d1`;
- production database: `~/.local/share/gaudere/state/state.db`, schema 4;
- current production service: `gaudere-agent.service`;
- WakeIntent acceptance profile: OFF.

The **primary rollback target for this promotion is the current production image `ea3dd924...`**, not the older historical rollback images. A promotion must not retag, remove, or overwrite any protected image.

Provider call #7 is durable evidence, not promotion authority:

- Task `cognition.current.v0:7e15434b8b3cde4d6a548872d6299afe327dd0469441ec6bcdd5961d6fa86bca`;
- exactly one confirmed Action `provider.call:openai.responses:<task-id>`;
- Task succeeded on attempt 1;
- provider budget moved 6 -> 7;
- canonical decision was `continue` and requested this plan.

## Phase 0 — preflight, no mutation

All conditions below are mandatory. Any mismatch means **do not promote**.

1. B10 executor is active and the command is delivered only through Sol's B10 mailbox.
2. `gaudere-agent.service` is active on the baseline image before the maintenance window.
3. `podman inspect gaudere-agent --format '{{.Image}}'`, normalized with or without the `sha256:` prefix, resolves to the baseline production image above.
4. Candidate inspection resolves to the exact candidate ID above and labels exactly the frozen Agent and Core revisions.
5. `scripts/verify-image-provenance.sh` passes for the candidate.
6. All four images — candidate plus the three protected baseline/rollback images — exist locally.
7. `PRAGMA user_version` is 4 and `PRAGMA integrity_check` is `ok`.
8. The installed production Quadlet/service contains no `--wake-intents` activation.
9. No Task is leased/running (`status IN (1,2)`) and no unexpected nonterminal provider/current-cognition work exists.
10. The call-#7 Task is succeeded exactly once and its provider Action is unique and confirmed.
11. Durable provider budget is exactly the value recorded immediately before promotion; promotion itself is not allowed to consume it.
12. Capture semantic baseline counts/state for `tasks`, `actions`, `budget_consumptions`, and `wake_intents` so startup can be checked for hidden effects.

## Phase 1 — stopped-state preservation

The service must be stopped before changing its image profile.

1. Stop only `gaudere-agent.service`.
2. Confirm it is inactive and acquire the same `state.db.lock` ownership used by the runtime/installer.
3. Run `scripts/backup-state.sh` while stopped. Record the archive path and `.sha256` sidecar and verify the sidecar immediately.
4. Preserve a copy/hash of the current `~/.config/containers/systemd/gaudere-agent.container` profile.
5. Re-read schema 4, `integrity_check=ok`, semantic baseline counts, baseline image identity, candidate identity, and protected-image presence.

No DB restore is part of the normal promotion path. The stopped-state backup is recovery evidence.

## Phase 2 — promotion boundary

The only intended configuration change is the immutable `Image=` value in the OpenAI Quadlet. No database row, provider budget, Action, Task, WakeIntent, secret, or network policy is intentionally changed.

Use the already-proved transactional installer from the exact candidate source revision, while the service is stopped, with `GAUDERE_IMAGE` resolving to the candidate. `scripts/install-openai-user-service.sh` resolves the supplied image to its immutable full ID before writing the Quadlet and rolls back an uncommitted profile replacement on failure.

Required installer outcome:

- installed `Image=` is exactly `sha256:9dd20dbacee908e3a760080c4495a38991c208ffc81e90e48066156ec46072a9`;
- OpenAI model remains `gpt-5.6-sol`;
- existing Podman secret name remains `gaudere-openai-api-key`;
- WakeIntent remains OFF;
- installer reports that no provider Task was submitted;
- service remains stopped until post-install inspection passes.

Do not mutate the friendly candidate tag during this boundary. The installed service must reference the immutable digest.

## Phase 3 — start and immediate health acceptance

Start `gaudere-agent.service` only after Phase 2 passes. Promotion is provisionally healthy only if all of the following pass:

1. `systemctl --user is-active gaudere-agent.service` returns `active`.
2. Container `gaudere-agent` resolves to candidate image `9dd20dbacee9...`.
3. Service/Quadlet still has no WakeIntent activation.
4. `PRAGMA user_version=4` and `PRAGMA integrity_check=ok`.
5. The semantic baseline from Phase 0 shows no spontaneous new provider budget row, Action, Task, or WakeIntent caused by promotion/startup.
6. Durable call #7 remains one succeeded Task plus one confirmed Action; no replay/reopen occurred.
7. `scripts/control-service.sh budget` returns normally without consuming budget.
8. `scripts/control-service.sh wake-status` returns normally and does not alter WakeIntent state.
9. `scripts/control-service.sh task cognition.current.v0:7e15434b8b3cde4d6a548872d6299afe327dd0469441ec6bcdd5961d6fa86bca` can read the durable successful #7 Task.
10. Recent service journal contains no startup crash/restart loop, state-lock failure, schema error, provider invocation, or unexpected task execution.

Observe the service again after a short bounded interval and repeat items 1–6. A transient first success is not enough if the unit enters a restart loop.

## Rollback triggers

Rollback immediately, without attempting a provider call, if any of these occurs during Phases 1–3:

- candidate ID or provenance differs from the frozen values;
- any protected image is missing;
- state lock cannot be acquired cleanly;
- stopped-state backup or its hash cannot be verified;
- schema differs from 4 or integrity check is not `ok`;
- installer fails or installed `Image=` is not the exact candidate digest;
- service fails to become/stay active or enters a restart loop;
- running container resolves to any image other than the candidate;
- WakeIntent becomes enabled;
- provider budget changes during promotion/startup;
- an unexpected provider Action, Task execution, or WakeIntent transition appears;
- durable #7 Task/Action evidence changes or becomes ambiguous;
- control read paths fail in a way indicating the new runtime cannot interpret production state.

## Rollback procedure

1. Stop `gaudere-agent.service` if it is running.
2. Preserve the failed candidate profile, journal excerpt, and current DB as forensic evidence before overwriting anything.
3. Re-run the transactional OpenAI service installer with `GAUDERE_IMAGE=sha256:ea3dd9247ae82c536468916b2da8a7da61e2721bfc0ec776a93068fecbc92b01` while stopped.
4. Verify installed `Image=` resolves to that exact baseline image.
5. Start the service and require the same health checks, now against the baseline image.
6. Do **not** automatically restore the DB merely because the image was rolled back. If semantic DB state changed unexpectedly, preserve the unexpected DB first; restore the stopped-state backup only as an explicit recovery action after determining that no durable external-effect evidence would be erased.
7. If baseline restart is not healthy, stop and escalate to manual review; do not cascade automatically to older rollback images.

The images `3102c736...` and `6f2dab2e...` remain protected secondary recovery points, not automatic fallback steps.

## Later durable one-shot cognition acceptance

A later cognition call is a **separate authority boundary** and is not part of promotion. When separately allowed, the promoted runtime's durable cognition path is accepted only if all of these hold:

1. Use succeeded Task #7 as the predecessor for the next `cognition.current.v0` cycle.
2. Create a newly captured bounded context snapshot and freeze a distinct deterministic current-cognition Task.
3. Immediately before execution, `--check` proves: Task nonterminal, canonical, context age <=15 minutes, provider budget `accepted`, and provider effects still zero for that Task.
4. Exactly one provider Action is created for exactly that Task and exactly one provider budget row is added.
5. Task executes at most once and becomes terminal on attempt 1; a provider effect of unknown/confirmed state is never replayed automatically.
6. A successful result is canonical `gaudere.cognition.resume-decision.v1` with decision `stop` or `continue`; `continue` includes exactly one bounded objective proposal.
7. Either `stop` or `continue` is an accepted cognition outcome. The result itself grants no deploy, shell, network, WakeIntent, tool, or successor authority.
8. Service remains healthy and its production image identity remains unchanged by the one-shot cognition call.

If any item fails after a provider Action may have occurred, preserve durable evidence and enter manual review. Never retry under a new Task identity to obtain a nicer answer.

## Completion criterion for this plan

This plan is complete when its exact identities and invariants are reviewed in CI/review and can be followed without reconstructing state from chat history. **Completing this plan performs no production deployment and no provider call.**
