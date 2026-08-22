# Schema v4 migration proof

This gate proves that a verified stopped-state schema-v3 backup can be migrated on
a disposable copy and independently restored as schema v3. It does **not** replace
the production state directory, change a Quadlet, start a service, accept a wake,
submit a Task, or invoke a provider.

The validator is:

```sh
sh scripts/validate-schema-v4-migration-copy.sh BACKUP_ARCHIVE [TASK_ID]
```

It must run from a Gaudere Agent Git checkout containing merged Agent gate 2
`a5a5fbb27af85faf584318bf8ddcfa290d3df5ad` and pinned Core
`c24c40b84a12e51515cee4611e3dc79e9fd83892`.

## Input authority

The only input is a stopped-state backup produced while `backup-state.sh` held the
same `state.db.lock` flock used by the Agent. A sibling `.sha256` file is mandatory.
Its manifest must contain exactly the one expected archive name and digest. The
validator copies that verified archive into a private temporary directory, rejects
duplicate or unsafe paths, links, unsupported member types, and any archived
`state.db.lock`, and never writes to the source archive.

For the real Fedora proof, the service must be stopped before creating the fresh
backup and remain stopped until the proof completes. Exact host commands are issued
only after this script and its CI tests merge; running this source-level gate does
not authorize that host action.

## Disposable transition

The validator extracts two independent copies:

- **migrated copy:** starts at schema v3, proves flock refusal, then runs exactly
  `--check --wake-intents`;
- **rollback copy:** remains on schema v3 and is reopened only through the default
  capability-disabled path.

The container path uses `Network=none`, a read-only root filesystem, no provider
model, and no secret. A direct test binary also receives no provider activation.

The migrated copy must reach schema v4 and contain the exact expected wake table,
index, and three immutability triggers with zero rows. A second opt-in check must be
idempotent. A subsequent default check must remain safe without enabling wake
behavior.

## Preservation proof

Before migration, the validator records a canonical snapshot of every pre-existing
non-SQLite table, column, row, index, trigger, and table definition. This includes
Task result metadata and all budget consumptions. SQLite `integrity_check` must pass.

That snapshot must remain byte-identical after migration, opt-in reopen, and default
reopen. The fixed provider budget row count can additionally be required with:

```sh
GAUDERE_EXPECT_PROVIDER_BUDGET_ROWS=3
```

The independently restored rollback copy must match the original snapshot, remain
schema v3, contain no wake table, survive a default `--check`, and retain an optional
representative Task report. The source archive checksum and bytes are checked again
after all validation.

## Deliberate non-goals

Passing this gate does not authorize:

- installing schema v4 as production state;
- adding `--wake-intents` to offline or OpenAI service profiles;
- starting production against a migrated copy;
- accepting or fabricating a `propose_wake` source;
- creating successor cognition;
- spending a provider permit.

Production directory replacement, service activation, live observation, and human
rollback remain a later explicit decision.
