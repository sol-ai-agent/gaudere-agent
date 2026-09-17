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

        auto invalid_first_predecessor = scheduled;
        invalid_first_predecessor.predecessor_task_id =
            "cognition.local-goose-cycle.v1:" + hex('c');
        invalid_first_predecessor.predecessor_result_sha256 = hex('d');
        assert(!valid_local_goose_cycle_cursor(invalid_first_predecessor));
        assert(!valid_local_goose_cycle_transition(seed, invalid_first_predecessor));

        auto first_blocked = scheduled;
        first_blocked.revision = 2;
        first_blocked.state = LocalGooseCycleState::blocked;
        first_blocked.blocked_reason = "first generation failure";
        assert(valid_local_goose_cycle_cursor(first_blocked));
        assert(valid_local_goose_cycle_transition(scheduled, first_blocked));

        auto invalid_first_blocked_predecessor = first_blocked;
        invalid_first_blocked_predecessor.predecessor_task_id =
            "cognition.local-goose-cycle.v1:" + hex('e');
        invalid_first_blocked_predecessor.predecessor_result_sha256 = hex('f');
        assert(!valid_local_goose_cycle_cursor(invalid_first_blocked_predecessor));
        assert(!valid_local_goose_cycle_transition(
            scheduled, invalid_first_blocked_predecessor));

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
        wrong_anchor.generation = 2;
        wrong_anchor.state = LocalGooseCycleState::dormant;
        wrong_anchor.anchor_observation_result_sha256 = hex('e');
        wrong_anchor.predecessor_task_id = prepared.current_task_id;
        wrong_anchor.predecessor_result_sha256 = hex('f');
        wrong_anchor.due_at_ms.reset();
        wrong_anchor.captured_at_ms.reset();
        wrong_anchor.current_task_id.clear();
        assert(valid_local_goose_cycle_cursor(wrong_anchor));
        assert(!valid_local_goose_cycle_transition(prepared, wrong_anchor));

        auto dormant = prepared;
        dormant.revision = 3;
        dormant.generation = 2;
        dormant.state = LocalGooseCycleState::dormant;
        dormant.predecessor_task_id = prepared.current_task_id;
        dormant.predecessor_result_sha256 = hex('f');
        dormant.due_at_ms.reset();
        dormant.captured_at_ms.reset();
        dormant.current_task_id.clear();
        assert(valid_local_goose_cycle_cursor(dormant));
        assert(valid_local_goose_cycle_transition(prepared, dormant));
        assert(store.replace(prepared, dormant).result
               == LocalGooseCycleStoreResult::accepted);

        auto invalid_dormant_generation_one = dormant;
        invalid_dormant_generation_one.generation = 1;
        invalid_dormant_generation_one.predecessor_task_id.reset();
        invalid_dormant_generation_one.predecessor_result_sha256.reset();
        assert(!valid_local_goose_cycle_cursor(invalid_dormant_generation_one));

        auto next = dormant;
        next.revision = 4;
        next.state = LocalGooseCycleState::scheduled;
        next.due_at_ms = 20'000;
        assert(valid_local_goose_cycle_transition(dormant, next));
        assert(store.replace(dormant, next).result
               == LocalGooseCycleStoreResult::accepted);

        auto skipped_generation = next;
        skipped_generation.generation = 3;
        assert(valid_local_goose_cycle_cursor(skipped_generation));
        assert(!valid_local_goose_cycle_transition(dormant, skipped_generation));

        auto blocked = next;
        blocked.revision = 5;
        blocked.state = LocalGooseCycleState::blocked;
        blocked.blocked_reason = "canonical predecessor mismatch";
        assert(valid_local_goose_cycle_cursor(blocked));
        assert(valid_local_goose_cycle_transition(next, blocked));

        auto mutated_generation = blocked;
        mutated_generation.generation = 3;
        assert(valid_local_goose_cycle_cursor(mutated_generation));
        assert(!valid_local_goose_cycle_transition(next, mutated_generation));

        auto mutated_predecessor = blocked;
        mutated_predecessor.predecessor_task_id =
            "cognition.local-goose-cycle.v1:" + hex('1');
        mutated_predecessor.predecessor_result_sha256 = hex('2');
        assert(valid_local_goose_cycle_cursor(mutated_predecessor));
        assert(!valid_local_goose_cycle_transition(next, mutated_predecessor));

        auto mutated_due = blocked;
        mutated_due.due_at_ms = 20'001;
        assert(valid_local_goose_cycle_cursor(mutated_due));
        assert(!valid_local_goose_cycle_transition(next, mutated_due));

        auto mutated_capture = blocked;
        mutated_capture.captured_at_ms = 20'000;
        assert(valid_local_goose_cycle_cursor(mutated_capture));
        assert(!valid_local_goose_cycle_transition(next, mutated_capture));

        auto mutated_task = blocked;
        mutated_task.current_task_id =
            "cognition.local-goose-cycle.v1:" + hex('3');
        assert(valid_local_goose_cycle_cursor(mutated_task));
        assert(!valid_local_goose_cycle_transition(next, mutated_task));

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
