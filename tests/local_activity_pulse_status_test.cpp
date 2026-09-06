#include "LocalActivityPulseStatus.hpp"

#include "LocalActivityPulseStore.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace gaudere_agent;
using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string repeated(const char value)
{
    return std::string(64, value);
}

std::string bytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void execute(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void bind_text(sqlite3* database, sqlite3_stmt* statement,
               const int index, const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}

void create_core_schema(sqlite3* database)
{
    execute(database, "PRAGMA journal_mode=WAL;");
    execute(database, "PRAGMA synchronous=FULL;");
    execute(database,
        "CREATE TABLE tasks ("
        " id TEXT PRIMARY KEY NOT NULL,"
        " idempotency_key TEXT NOT NULL UNIQUE,"
        " kind TEXT NOT NULL,"
        " input_content_type TEXT NOT NULL,"
        " input TEXT NOT NULL,"
        " max_input_bytes INTEGER NOT NULL,"
        " max_output_bytes INTEGER NOT NULL,"
        " max_runtime_ms INTEGER NOT NULL,"
        " max_attempts INTEGER NOT NULL,"
        " attempts_started INTEGER NOT NULL,"
        " status INTEGER NOT NULL,"
        " lease_owner TEXT,"
        " lease_expires_at_ms INTEGER,"
        " cancel_reason TEXT NOT NULL,"
        " result_content_type TEXT,"
        " result_output TEXT,"
        " result_failure_code TEXT,"
        " result_failure_message TEXT,"
        " result_metadata_content_type TEXT,"
        " result_metadata TEXT"
        ");");
    execute(database, "PRAGMA user_version=4;");
}

void insert_task(sqlite3* database, const Task& task)
{
    Statement statement(database,
        "INSERT INTO tasks (id,idempotency_key,kind,input_content_type,input,"
        "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
        "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
        "result_content_type,result_output,result_failure_code,result_failure_message,"
        "result_metadata_content_type,result_metadata) "
        "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,NULL,NULL,?12,"
        "?13,?14,?15,?16,NULL,NULL)");
    bind_text(database, statement.get(), 1, task.id);
    bind_text(database, statement.get(), 2, task.idempotency_key);
    bind_text(database, statement.get(), 3, task.kind);
    bind_text(database, statement.get(), 4, task.input_content_type);
    bind_text(database, statement.get(), 5, task.input);
    sqlite3_bind_int64(statement.get(), 6,
        static_cast<sqlite3_int64>(task.limits.max_input_bytes));
    sqlite3_bind_int64(statement.get(), 7,
        static_cast<sqlite3_int64>(task.limits.max_output_bytes));
    sqlite3_bind_int64(statement.get(), 8, task.limits.max_runtime.count());
    sqlite3_bind_int64(statement.get(), 9, task.limits.max_attempts);
    sqlite3_bind_int64(statement.get(), 10, task.attempts_started);
    sqlite3_bind_int(statement.get(), 11, static_cast<int>(task.status));
    bind_text(database, statement.get(), 12, task.cancel_reason);
    if (!task.result) throw std::runtime_error("test Task must have a result");
    bind_text(database, statement.get(), 13, task.result->content_type);
    bind_text(database, statement.get(), 14, task.result->output);
    bind_text(database, statement.get(), 15, task.result->failure_code);
    bind_text(database, statement.get(), 16, task.result->failure_message);
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(database));
}

LocalContinuityObservationFacts facts(const std::int64_t due,
                                      const std::int64_t captured)
{
    LocalContinuityObservationFacts value;
    value.generation = 1;
    value.due_at_ms = due;
    value.captured_at_ms = captured;
    value.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + repeated('a');
    value.anchor_checkpoint_result_sha256 = repeated('b');
    value.provider_scope = "provider.call:openai.responses";
    value.provider_total = 10;
    value.provider_limit = 12;
    value.predecessor_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + repeated('c');
    value.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + repeated('d');
    value.historical_wake_scope = "cognition.reflect.wake.v0";
    value.historical_wake_sha256 = repeated('e');
    return value;
}

LocalActivityPulseCursor seed_cursor(const LocalContinuityObservationFacts& value,
                                     const std::int64_t anchor_at)
{
    LocalActivityPulseCursor cursor;
    cursor.anchor_checkpoint_task_id = value.anchor_checkpoint_task_id;
    cursor.anchor_checkpoint_result_sha256 = value.anchor_checkpoint_result_sha256;
    cursor.anchor_at_ms = anchor_at;
    cursor.due_at_ms = value.due_at_ms;
    return cursor;
}

} // namespace

int main()
{
    const fs::path root = fs::temp_directory_path()
        / ("gaudere-local-status-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    const fs::path absent_state = root / "absent-state.db";
    const fs::path absent_sidecar = root / "absent-sidecar.db";
    const auto unseeded = inspect_local_activity_pulse_status(
        absent_state.string(), absent_sidecar.string(), true);
    expect(unseeded.eligible, "unseeded status succeeds without touching Core DB");
    expect(!fs::exists(absent_state) && !fs::exists(absent_sidecar),
           "unseeded status creates neither state nor sidecar file");
    if (unseeded.eligible) {
        const auto parsed = Json::parse(unseeded.canonical_json);
        expect(parsed.at("schema") == local_activity_pulse_status_schema,
               "unseeded status uses canonical schema");
        expect(parsed.at("enabled") == true && parsed.at("seeded") == false,
               "enabled-but-unseeded remains explicitly inert");
        expect(parsed.at("scheduler_active") == false
                   && parsed.at("cursor_state") == "unseeded",
               "unseeded status exposes no deadline");
    }

    const fs::path state = root / "state.db";
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(state.c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::cerr << "cannot create status fixture Core DB\n";
        return 1;
    }
    create_core_schema(database);

    const auto observed = facts(2'000, 2'500);
    auto task = make_local_continuity_observation_task(observed);
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        local_continuity_observation_content_type, task.input, "", "", "", ""};
    execute(database, "BEGIN IMMEDIATE;");
    insert_task(database, task);
    execute(database, "COMMIT;");

    const fs::path sidecar = root / "local-activity.db";
    {
        LocalActivityPulseStore store(sidecar.string());
        const auto idle = seed_cursor(observed, 1'000);
        const auto seeded = store.seed(idle);
        expect(seeded.result == LocalActivityPulseStoreResult::accepted,
               "status fixture sidecar seeds canonically");
        if (!seeded.cursor) return 1;

        auto preparing = *seeded.cursor;
        preparing.revision += 1;
        preparing.generation = 1;
        preparing.state = LocalActivityPulseState::preparing;
        preparing.captured_at_ms = observed.captured_at_ms;
        preparing.task_id = task.id;
        const auto admitted = store.replace(*seeded.cursor, preparing);
        expect(admitted.result == LocalActivityPulseStoreResult::accepted,
               "status fixture enters preparing canonically");
        if (!admitted.cursor) return 1;

        auto settled = *admitted.cursor;
        settled.revision += 1;
        settled.state = LocalActivityPulseState::settled;
        settled.result_sha256 = sha256_hex(task.result->output);
        const auto finished = store.replace(*admitted.cursor, settled);
        expect(finished.result == LocalActivityPulseStoreResult::accepted,
               "status fixture settles canonically");
    }

    const fs::path wal = state.string() + "-wal";
    const fs::path shm = state.string() + "-shm";
    expect(fs::exists(wal) && fs::file_size(wal) > 0 && fs::exists(shm),
           "Core fixture keeps a live non-empty WAL and SHM");
    const auto state_before = bytes(state);
    const auto wal_before = bytes(wal);
    const auto sidecar_before = bytes(sidecar);

    const auto status = inspect_local_activity_pulse_status(
        state.string(), sidecar.string(), true);
    expect(status.eligible, "settled status reads live WAL state read-only");
    expect(bytes(state) == state_before && bytes(wal) == wal_before
               && bytes(sidecar) == sidecar_before,
           "status mutates neither Core DB/WAL nor pulse sidecar");
    expect(!fs::exists(sidecar.string() + "-journal")
               && !fs::exists(sidecar.string() + "-wal")
               && !fs::exists(sidecar.string() + "-shm"),
           "status creates no sidecar journal/WAL/SHM");

    if (status.eligible) {
        const auto parsed = Json::parse(status.canonical_json);
        expect(parsed.at("seeded") == true && parsed.at("enabled") == true,
               "status exposes enabled seeded source intent");
        expect(parsed.at("cursor_state") == "settled"
                   && parsed.at("generation") == 1
                   && parsed.at("revision") == 2,
               "status exposes durable cursor state and generation");
        expect(parsed.at("task_id") == task.id
                   && parsed.at("task_status") == "succeeded",
               "status exposes exact local observation Task");
        expect(parsed.at("result_sha256") == sha256_hex(task.result->output),
               "status exposes durable linked result hash");
        expect(parsed.at("latest_payload").at("schema")
                   == local_continuity_observation_schema
                   && parsed.at("latest_payload").at("provider_total") == 10,
               "status exposes inspected canonical latest payload");
        expect(parsed.at("next_deadline_ms")
                   == observed.captured_at_ms + local_activity_pulse_cadence_ms,
               "status exposes exact Scheduler-derived next deadline");
    }

    execute(database, "BEGIN IMMEDIATE;");
    {
        Statement corrupt(database,
            "UPDATE tasks SET result_output='corrupted' WHERE id=?1");
        bind_text(database, corrupt.get(), 1, task.id);
        if (sqlite3_step(corrupt.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database));
    }
    execute(database, "COMMIT;");
    const auto conflict = inspect_local_activity_pulse_status(
        state.string(), sidecar.string(), true);
    expect(!conflict.eligible,
           "status fails closed on a conflicting succeeded Task result");

    sqlite3_close(database);
    database = nullptr;

    sqlite3* sidecar_db = nullptr;
    if (sqlite3_open_v2(sidecar.c_str(), &sidecar_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::cerr << "cannot reopen status fixture sidecar\n";
        return 1;
    }
    execute(sidecar_db,
        "INSERT INTO local_activity_pulse_cursor "
        "SELECT 'extra.scope',revision,generation,state,anchor_checkpoint_task_id,"
        "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
        "task_id,result_sha256,predecessor_observation_task_id,"
        "predecessor_observation_result_sha256,blocked_reason "
        "FROM local_activity_pulse_cursor LIMIT 1");
    sqlite3_close(sidecar_db);
    const auto ambiguous = inspect_local_activity_pulse_status(
        state.string(), sidecar.string(), true);
    expect(!ambiguous.eligible,
           "status fails closed on an extra sidecar cursor row");

    fs::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " local activity status test(s) failed\n";
        return 1;
    }
    std::cout << "All local activity status tests passed\n";
    return 0;
}
