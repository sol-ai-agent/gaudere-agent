# Schema-v4 image provenance and rollback identity

Status: **PREP ONLY / NOT AUTHORIZED FOR PRODUCTION**.

This gate makes runtime-image identity explicit before the reversible schema-v4
deployment. It does not change an installed Quadlet, touch production state, use a
secret, call a provider, consume a permit, or accept/fire/revoke a WakeIntent.

## Four identities, four different facts

| Identity | Authoritative evidence | What it does not prove |
| --- | --- | --- |
| Agent source | Clean checkout `HEAD` and OCI labels | That any mutable image tag still points to it |
| Core source | Exact 40-byte `gaudere.ref` and OCI label | That the Agent checkout is clean |
| Candidate image | Full `sha256:...` image ID plus both revision labels | That it is installed or active |
| Pre-v4 rollback image | Dedicated tag paired with its captured full image ID and manifest | That production state has been migrated |

`main`, a candidate image, `localhost/gaudere-agent:dev`, the image referenced by
the installed service, and the production SQLite state are deliberately separate
facts. A merge changes source only. A build creates an image only. Moving a tag
changes a name only. None of those operations migrates or activates production.

## Build contract

`scripts/build-image.sh` now requires a clean Git checkout, reads the exact Agent
`HEAD` and Core pin, supplies both to the `Containerfile`, and verifies the built
image before returning success. The final image carries:

- `org.opencontainers.image.revision` = Agent commit;
- `io.gaudere.agent.revision` = the same Agent commit;
- `io.gaudere.core.revision` = exact `gaudere.ref` commit.

The normal development default remains `localhost/gaudere-agent:dev`. A schema-v4
candidate must use a distinct explicit tag:

```sh
GAUDERE_IMAGE_TAG=localhost/gaudere-agent:candidate-schema-v4-<agent-short-sha> \
    sh scripts/build-image.sh
```

The builder rejects a dirty checkout because labels would otherwise describe a
commit while uncommitted bytes were copied into the image.

## Rollback must be captured before the candidate build

The old mutable `:dev` image is protected first. The rollback identity is a pair:
a dedicated tag for human recovery and the immutable ID that the tag must continue
to resolve to. The capture also publishes a mode-0600 manifest without overwriting
an existing manifest or rollback tag.

```sh
GAUDERE_CURRENT_IMAGE=localhost/gaudere-agent:dev \
GAUDERE_ROLLBACK_IMAGE=localhost/gaudere-agent:rollback-before-schema-v4-<stamp> \
GAUDERE_ROLLBACK_MANIFEST=/explicit/safe/path/image-rollback-<stamp>.manifest \
    sh scripts/capture-schema-v4-image-rollback.sh
```

The script resolves `:dev` first and tags that full ID, never the mutable name.
Only after `rollback_capture=PASS` may a candidate build begin. Existing rollback
tags are never replaced.

## Reusable fail-closed checks

The small verifier is the contract consumed by later gates:

```sh
sh scripts/verify-image-provenance.sh \
    IMAGE EXPECTED_AGENT_REF EXPECTED_CORE_REF EXPECTED_IMAGE_ID
```

It requires the tag to resolve to the expected full image ID and requires all
Agent/Core labels to match exactly. Missing labels, abbreviated IDs, wrong commits,
or tag drift fail closed.

The composite disposable proof combines that identity check with the existing
schema-v4 copy validator:

```sh
GAUDERE_CANDIDATE_IMAGE=localhost/gaudere-agent:candidate-schema-v4-... \
GAUDERE_EXPECTED_AGENT_REF=<40-character-agent-commit> \
GAUDERE_EXPECTED_CORE_REF=<40-character-core-commit> \
GAUDERE_EXPECTED_CANDIDATE_ID=sha256:<64-hex-digest> \
GAUDERE_ROLLBACK_IMAGE=localhost/gaudere-agent:rollback-before-schema-v4-... \
GAUDERE_EXPECTED_ROLLBACK_ID=sha256:<64-hex-digest> \
    sh scripts/validate-schema-v4-image-provenance.sh \
       /path/to/verified-backup.tar.gz [REPRESENTATIVE_TASK_ID]
```

It proves, on disposable restores only:

1. exact candidate Agent/Core provenance and immutable ID;
2. exact retained rollback tag/ID;
3. candidate opening an untouched schema-v3 copy;
4. candidate migrating only a disposable copy to v4 with the already-reviewed
   opt-in migration path;
5. candidate default-reopening v4 without `--wake-intents`;
6. zero WakeIntent rows/effects and unchanged Tasks, Actions, provider metadata,
   and durable provider-budget rows;
7. candidate and rollback tags still resolve to the same IDs after the proof.

The proof uses `--network none`, mounts no provider secret, invokes no provider
task, and never receives the production state path.

## CI and failure cases

Synthetic tests reject false Agent/Core labels, a candidate tag already derived to
another image, candidate drift during validation, rollback drift during validation,
and attempted rollback-tag overwrite. The image workflow additionally builds the
real candidate and executes the disposable v3/v4 reopen proof.

This gate produces evidence for the staged deployment; it does not authorize that
deployment. PR #52 must consume the verifier and exact IDs after this contract is
merged rather than reimplementing provenance logic.
