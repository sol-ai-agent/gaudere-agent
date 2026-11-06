# Retry generation 1 — existing-only SQLite read-only opener

Status: **code-only**, no runtime, no Fedora operation, no production activation. Follows the operator-reported production audit PASS for #269 and the code-only evidence inspector merged in #271.

## Motivation

The existing `LocalGooseDialogueResponderRetryStore(path)` constructor opens a writable connection, can create the sidecar when absent, sets SQLite pragmas, and runs `BEGIN IMMEDIATE`. A nominally const reference to that object does **not** guarantee that opening it performed no writes. Production read-only inspection must never use that constructor.

## Explicit mode

`LocalGooseDialogueResponderRetryStore(path, LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only)`

- Requires **already existing** current-user regular file mode `0600`. Rejects missing files, symlinks, wrong owner and wrong mode.
- Rejects a snapshot if `-wal`, `-shm` or `-journal` companion exists (including dangling companion symlinks). This first reader accepts **quiescent rollback-journal snapshots only**; it does not recover an active database.
- Opens SQLite with `SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX`, checks `sqlite3_db_readonly(..., "main") == 1`.
- Reads `PRAGMA user_version` and `sqlite_master`, requires exact schema v1 and the expected sole lineage table; requires `PRAGMA journal_mode` to report `delete`.
- Performs no schema creation/upgrade, `BEGIN IMMEDIATE`, write pragma, journal change or production state normalization.
- Any call to `prepare_lineage` on this instance returns `unavailable` immediately; the existing writable default constructor and its behavior are left unchanged for separately gated admission tooling.
- Exposes `is_read_only()` for callers/tests. This is a **runtime mode**; callers needing a compile-time read-only type should still limit access to const references as in #271.

## Constraints and future hardening

Only a **fixed, quiescent and separately copied** SQLite state is authorized for proof of this mode in this tranche. An actively written production database, WAL snapshot or mutable live-path race requires a different contract. In particular, this API is not a license to read or alter active production state from an autonomous agent. An authenticated / typed / strictly bounded production inspection operation must be separately designed and gated, and verify its own source snapshots and initial conditions.

The direct filesystem path trust model assumes the directory is under the trusted current user's control; this code does not claim to solve hostile same-UID filesystem races. Never pass an attacker-controlled path.

## Tests and isolation

Fedora 44 `-Werror` checks the original retry store proof plus new assertions:
- exact v1 lineage remains readable, but prepare is refused and original byte snapshot is identical;
- no `-wal/-shm/-journal` created;
- absent file not created; wrong mode and symlink rejected;
- companion `-wal` rejected without recovery;
- unversioned file not upgraded; future schema not opened;
- original writer/default behavior still succeeds, preserving the #265–#269 helpers.

No `src/main.cpp`, `src/Makefile.am` or production `Containerfile` wiring. No grant, lease, decision, intent, disposition, submission Task, ACK, preferred-head mutation, provider or dispatch.

For future independent gates: staged readonly code validation -> CI green -> merge -> explicitly authorized frozen Fedora copy inspection -> verified hashes/no sidecars/held sequence 4. **None of those later host steps are performed by this PR.**
