#include "LocalGooseCycleStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace gaudere_agent;

std::string hex(const char c) { return std::string(64, c); }

LocalGooseCycleCursor seed_cursor()
{
    LocalGooseCycleCursor cursor;
    cursor.anchor_observation_task_id =
        "continuity.local-observation.v1:" + hex('a');
    cursor.anchor_observation_result_sha256 = hex('b');
    return cursor;
}

std::string temporary_path(const char* suffix)
{
    return std::string{"/tmp/gaudere-local-goose-cycle-sidecar-"}
        + std::to_string(static_cast<long long>(getpid())) + suffix;
}

} // namespace

int main()
{
    const auto path = temporary_path(".db");
    std::remove(path.c_str());

    const auto seed = seed_cursor();
    assert(valid_local_goose_cycle_cursor(seed));

    {
        LocalGooseCycleStore store(path);
        struct stat status {};
        assert(stat(path.c_str(), &status) == 0);
        assert((status.st_mode & 0777) == 0600);

        const auto first = store.seed(seed);
        assert(first.result == LocalGooseCycleStoreResult::accepted);
        assert(first.cursor && first.cursor->revision == 0);

        const auto duplicate = store.seed(seed);
        assert(duplicate.result == LocalGooseCycleStoreResult::duplicate);

        auto conflicting_seed = seed;
        conflicting_seed.anchor_observation_result_sha256 = hex('c');
        const auto conflict = store.seed(conflicting_seed);
        assert(conflict.result == LocalGooseCycleStoreResult::conflict);

        auto invalid_seed = seed;
        invalid_seed.generation = 1;
        const auto invalid = store.seed(invalid_seed);
        assert(invalid.result == LocalGooseCycleStoreResult::invalid);

        auto scheduled = seed;
        scheduled.revision = 1;
        scheduled.generation = 1;
        scheduled.state = LocalGooseCycleState::scheduled;
        scheduled.due_at_ms = 10'000;
        assert(valid_local_goose_cycle_transition(seed, scheduled));
        const auto scheduled_write = store.replace(seed, scheduled);
        assert(scheduled_write.result == LocalGooseCycleStoreResult::accepted);

        auto alternate = seed;
        alternate.revision = 1;
        alternate.generation = 1;
        alternate.state = LocalGooseCycleState::scheduled;
        alternate.due_at_ms = 20'000;
        const auto stale = store.replace(seed, alternate);
        assert(stale.result == LocalGooseCycleStoreResult::conflict);

        auto prepared = scheduled;
        prepared.revision = 2;
        prepared.state = LocalGooseCycleState::prepared;
        prepared.captured_at_ms = 10'001;
        prepared.current_task_id = "cognition.local-goose-cycle.v1:" + hex('d');
        assert(valid_local_goose_cycle_transition(scheduled, prepared));
        assert(store.replace(scheduled, prepared).result
               == LocalGooseCycleStoreResult::accepted);

        auto wrong_anchor = prepared;
        wrong_anchor.revision = 3;
        wrong_anchor.state = LocalGooseCycleState::dormant;
        wrong_anchor.anchor_observation_result_sha256 = hex('e');
        wrong_anchor.predecessor_task_id = prepared.current_task_id;
        wrong_anchor.predecessor_result_sha256 = hex('f');
        wrong_anchor.due_at_ms.reset();
        wrong_anchor.captured_at_ms.reset();
        wrong_anchor.current_task_id.clear();
        assert(!valid_local_goose_cycle_transition(prepared, wrong_anchor));

        auto dormant = prepared;
        dormant.revision = 3;
        dormant.state = LocalGooseCycleState::dormant;
        dormant.predecessor_task_id = prepared.current_task_id;
        dormant.predecessor_result_sha256 = hex('f');
        dormant.due_at_ms.reset();
        dormant.captured_at_ms.reset();
        dormant.current_task_id.clear();
        assert(valid_local_goose_cycle_transition(prepared, dormant));
        assert(store.replace(prepared, dormant).result
               == LocalGooseCycleStoreResult::accepted);

        auto next = dormant;
        next.revision = 4;
        next.generation = 2;
        next.state = LocalGooseCycleState::scheduled;
        next.due_at_ms = 20'000;
        assert(valid_local_goose_cycle_transition(dormant, next));
        assert(store.replace(dormant, next).result
               == LocalGooseCycleStoreResult::accepted);

        auto blocked = next;
        blocked.revision = 5;
        blocked.state = LocalGooseCycleState::blocked;
        blocked.blocked_reason = "canonical predecessor mismatch";
        assert(valid_local_goose_cycle_transition(next, blocked));
        assert(store.replace(next, blocked).result
               == LocalGooseCycleStoreResult::accepted);

        auto impossible = blocked;
        impossible.revision = 6;
        assert(!valid_local_goose_cycle_transition(blocked, impossible));
    }

    const auto inspection = inspect_local_goose_cycle_sidecar(path);
    assert(inspection.eligible);
    assert(inspection.cursor);
    assert(inspection.cursor->state == LocalGooseCycleState::blocked);
    assert(inspection.cursor->generation == 2);

    chmod(path.c_str(), 0644);
    const auto insecure = inspect_local_goose_cycle_sidecar(path);
    assert(!insecure.eligible);
    chmod(path.c_str(), 0600);

    sqlite3* database = nullptr;
    assert(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database, "CREATE TABLE unexpected(x INTEGER);",
                        nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
    const auto extra_table = inspect_local_goose_cycle_sidecar(path);
    assert(!extra_table.eligible);

    std::remove(path.c_str());
    std::cout << "local Goose cycle sidecar: ok\n";
    return 0;
}
