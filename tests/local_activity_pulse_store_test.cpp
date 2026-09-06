#include "LocalActivityPulseStore.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string temporary_path()
{
    char pattern[] = "/tmp/gaudere-local-activity-XXXXXX";
    const int descriptor = mkstemp(pattern);
    if (descriptor < 0) throw std::runtime_error("mkstemp failed");
    close(descriptor);
    unlink(pattern);
    return pattern;
}

std::string repeated(const char value)
{
    return std::string(64, value);
}

struct FileSnapshot {
    bool exists = false;
    off_t size = 0;
    timespec modified{};
};

FileSnapshot file_snapshot(const std::string& path)
{
    struct stat status {};
    if (stat(path.c_str(), &status) == 0)
        return {true, status.st_size, status.st_mtim};
    if (errno == ENOENT) return {};
    throw std::runtime_error("stat failed for " + path);
}

bool same_snapshot(const FileSnapshot& left, const FileSnapshot& right) noexcept
{
    return left.exists == right.exists
        && (!left.exists
            || (left.size == right.size
                && left.modified.tv_sec == right.modified.tv_sec
                && left.modified.tv_nsec == right.modified.tv_nsec));
}

gaudere_agent::LocalActivityPulseCursor seed_cursor()
{
    gaudere_agent::LocalActivityPulseCursor cursor;
    cursor.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + repeated('a');
    cursor.anchor_checkpoint_result_sha256 = repeated('b');
    cursor.anchor_at_ms = 1'000;
    cursor.due_at_ms = 86'401'000;
    return cursor;
}

gaudere_agent::LocalActivityPulseCursor preparing_one(
    const gaudere_agent::LocalActivityPulseCursor& seed)
{
    auto cursor = seed;
    cursor.revision = 1;
    cursor.generation = 1;
    cursor.state = gaudere_agent::LocalActivityPulseState::preparing;
    cursor.captured_at_ms = seed.due_at_ms + 25;
    cursor.task_id = "continuity.local-observation.v1:" + repeated('c');
    return cursor;
}

void execute_sql(const std::string& path, const std::string& sql)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database)
                                             : "sqlite open failed";
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    char* error = nullptr;
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        sqlite3_close(database);
        throw std::runtime_error(message);
    }
    sqlite3_close(database);
}

void remove_sqlite_files(const std::string& path)
{
    unlink((path + "-shm").c_str());
    unlink((path + "-wal").c_str());
    unlink(path.c_str());
}

} // namespace

int main()
{
    using namespace gaudere_agent;

    const auto path = temporary_path();
    const auto seed = seed_cursor();
    expect(valid_local_activity_pulse_cursor(seed),
           "canonical idle seed cursor is valid");

    {
        LocalActivityPulseStore store(path);
        struct stat status {};
        expect(stat(path.c_str(), &status) == 0
                   && S_ISREG(status.st_mode)
                   && status.st_uid == geteuid()
                   && (status.st_mode & 0777) == 0600,
               "sidecar is a current-user mode 0600 regular file");

        const auto first = store.seed(seed);
        expect(first.result == LocalActivityPulseStoreResult::accepted,
               "first canonical seed is accepted");
        expect(store.seed(seed).result == LocalActivityPulseStoreResult::duplicate,
               "identical seed replay is idempotent");

        auto conflicting_seed = seed;
        conflicting_seed.anchor_checkpoint_result_sha256 = repeated('d');
        expect(store.seed(conflicting_seed).result
                   == LocalActivityPulseStoreResult::conflict,
               "same fixed scope cannot be rebound to a different checkpoint anchor");

        const auto preparing = preparing_one(seed);
        expect(valid_local_activity_pulse_transition(seed, preparing),
               "idle to generation-1 preparing transition is canonical");

        auto before_due = preparing;
        before_due.captured_at_ms = before_due.due_at_ms - 1;
        expect(!valid_local_activity_pulse_cursor(before_due)
                   && !valid_local_activity_pulse_transition(seed, before_due),
               "capture before durable due time is rejected");

        expect(store.replace(seed, preparing).result
                   == LocalActivityPulseStoreResult::accepted,
               "first CAS transition is accepted");
        expect(store.replace(seed, preparing).result
                   == LocalActivityPulseStoreResult::duplicate,
               "exact CAS replay after commit converges as duplicate");

        auto stale_different = preparing;
        stale_different.task_id =
            "continuity.local-observation.v1:" + repeated('e');
        expect(store.replace(seed, stale_different).result
                   == LocalActivityPulseStoreResult::conflict,
               "stale writer with different semantic state fails closed");

        auto settled = preparing;
        settled.revision = 2;
        settled.state = LocalActivityPulseState::settled;
        settled.result_sha256 = repeated('f');
        expect(valid_local_activity_pulse_transition(preparing, settled),
               "preparing to settled transition is canonical");
        expect(store.replace(preparing, settled).result
                   == LocalActivityPulseStoreResult::accepted,
               "settlement CAS is accepted");

        auto preparing_two = settled;
        preparing_two.revision = 3;
        preparing_two.generation = 2;
        preparing_two.state = LocalActivityPulseState::preparing;
        preparing_two.due_at_ms = *settled.captured_at_ms + 86'400'000;
        preparing_two.captured_at_ms = preparing_two.due_at_ms + 10;
        preparing_two.predecessor_observation_task_id = settled.task_id;
        preparing_two.predecessor_observation_result_sha256 =
            *settled.result_sha256;
        preparing_two.task_id =
            "continuity.local-observation.v1:" + repeated('1');
        preparing_two.result_sha256.reset();
        expect(valid_local_activity_pulse_transition(settled, preparing_two),
               "settled generation advances by exactly one to preparing");

        auto illegal_anchor = preparing_two;
        illegal_anchor.anchor_checkpoint_task_id =
            "continuity.delta-checkpoint.v1:" + repeated('2');
        expect(!valid_local_activity_pulse_transition(settled, illegal_anchor),
               "checkpoint anchor cannot change across CAS");

        auto generation_jump = preparing_two;
        generation_jump.generation = 3;
        expect(!valid_local_activity_pulse_transition(settled, generation_jump),
               "generation cannot skip an opportunity");
    }

    const auto database_before = file_snapshot(path);
    const auto wal_before = file_snapshot(path + "-wal");
    const bool shm_before = file_snapshot(path + "-shm").exists;
    const auto inspected = inspect_local_activity_pulse_sidecar(path);
    const auto database_after = file_snapshot(path);
    const auto wal_after = file_snapshot(path + "-wal");
    const bool shm_after = file_snapshot(path + "-shm").exists;
    expect(inspected.eligible && inspected.cursor
               && inspected.cursor->generation == 1
               && inspected.cursor->state == LocalActivityPulseState::settled,
           "strict read-only inspector returns the one canonical durable cursor");
    expect(same_snapshot(database_before, database_after),
           "read-only inspection does not modify the main SQLite database");
    expect(same_snapshot(wal_before, wal_after),
           "read-only inspection neither creates nor modifies WAL state");
    if (shm_before != shm_after) {
        std::cout << "SQLite read-only inspection changed only ephemeral -shm presence\n";
    }

    const auto ambiguous_path = temporary_path();
    {
        LocalActivityPulseStore store(ambiguous_path);
        expect(store.seed(seed).result == LocalActivityPulseStoreResult::accepted,
               "ambiguous-sidecar fixture seeds canonically");
    }
    execute_sql(ambiguous_path,
        "INSERT INTO local_activity_pulse_cursor "
        "SELECT 'other-scope',revision,generation,state,anchor_checkpoint_task_id,"
        "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
        "task_id,result_sha256,predecessor_observation_task_id,"
        "predecessor_observation_result_sha256,blocked_reason "
        "FROM local_activity_pulse_cursor WHERE scope='continuity.local-observation-pulse.v1'");
    expect(!inspect_local_activity_pulse_sidecar(ambiguous_path).eligible,
           "read-only inspector rejects an extra cursor row");

    const auto extra_table_path = temporary_path();
    {
        LocalActivityPulseStore store(extra_table_path);
        expect(store.seed(seed).result == LocalActivityPulseStoreResult::accepted,
               "extra-table fixture seeds canonically");
    }
    execute_sql(extra_table_path, "CREATE TABLE unexpected(value INTEGER)");
    expect(!inspect_local_activity_pulse_sidecar(extra_table_path).eligible,
           "read-only inspector rejects an extra user table");

    const auto bad_mode_path = temporary_path();
    const int descriptor = open(bad_mode_path.c_str(),
                                O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor >= 0) close(descriptor);
    chmod(bad_mode_path.c_str(), 0644);
    bool bad_mode_rejected = false;
    try {
        LocalActivityPulseStore store(bad_mode_path);
    } catch (const std::runtime_error&) {
        bad_mode_rejected = true;
    }
    expect(bad_mode_rejected,
           "writer refuses a sidecar with group/other permissions");
    expect(!inspect_local_activity_pulse_sidecar(bad_mode_path).eligible,
           "read-only inspector also refuses unsafe sidecar permissions");

    remove_sqlite_files(path);
    remove_sqlite_files(ambiguous_path);
    remove_sqlite_files(extra_table_path);
    remove_sqlite_files(bad_mode_path);

    if (failures != 0) {
        std::cerr << failures << " local activity pulse store test(s) failed\n";
        return 1;
    }
    std::cout << "All local activity pulse store tests passed\n";
    return 0;
}
