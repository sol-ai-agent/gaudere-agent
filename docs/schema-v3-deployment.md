# Schema v3 production deployment

The v2 -> v3 transition is deliberately deployed by **directory replacement**, not by
modifying the sole production `state.db` in place.

`scripts/deploy-schema-v3.sh [TASK_ID]` is intended to run only while the ordinary
`gaudere-agent.service` is stopped. It:

1. refuses to continue while the user service is active/activating/reloading;
2. verifies the state advisory lock is free;
3. requires the live database to be schema v2;
4. creates and verifies a fresh stopped-state backup;
5. restores that backup into a staging directory on the same filesystem;
6. runs the current agent on the staging copy with `Network=none`, no provider secret,
   and no provider model activation;
7. requires staging schema v3, unchanged logical Task/Action/Budget rows, and no
   fabricated metadata on historical Tasks;
8. optionally inspects a representative durable Task both before and after installation;
9. rechecks service inactivity, the state lock, schema v2, and unchanged logical rows
   immediately before replacement;
10. renames the original `state/` directory to a timestamped `state.pre-v3-*` rollback
    directory and installs the already-validated staging directory as the new `state/`;
11. validates the installed schema and optional Task before declaring the transition
    ready.

If any post-replacement validation fails, the script automatically moves the failed v3
state aside as `state.failed-v3-*` and restores the original v2 directory to `state/`.

A successful run deliberately leaves the service stopped. The operator must explicitly
start it and verify service health, the representative Task, and the live provider
budget before the rollback directory is considered removable.

The fresh backup archive and `state.pre-v3-*` directory are rollback material and must
not be deleted during the first production validation.

This transition changes only persistence schema capability. It does **not** enable
OpenAI, mount the real secret, or alter the offline `Network=none` Quadlet boundary.
