#include "LocalActivityPulseStatus.hpp"

#include "LocalActivityPulseSchedulerBridge.hpp"
#include "LocalActivityPulseStore.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using TaskResult = gaudere::work::TaskResult;
using TimePoint = gaudere::work::TimePoint;

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database));
    }
    ~Statement() { sqlite3_finalize(statement_); }
    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

class ReadOnlyCoreDatabase {
public:
    explicit ReadOnlyCoreDatabase(const std::string& path)
    {
        struct stat metadata {};
        if (::lstat(path.c_str(), &metadata) != 0
            || !S_ISREG(metadata.st_mode) || S_ISLNK(metadata.st_mode)
            || metadata.st_uid != ::geteuid()) {
            throw std::runtime_error(
                "Core state database must pre-exist as a current-user regular file");
        }

        const bool wal_has_frames = wal_has_frames_with_readable_shm(path);
        const std::string uri = readonly_uri(path, !wal_has_frames);
        if (sqlite3_open_v2(uri.c_str(), &database_,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_URI
                                | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database_ ? sqlite3_errmsg(database_)
                                                   : "cannot open Core state read-only";
            sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
        if (sqlite3_exec(database_, "PRAGMA query_only=ON; BEGIN;",
                         nullptr, nullptr, nullptr) != SQLITE_OK) {
            const std::string message = sqlite3_errmsg(database_);
            sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
    }

    ~ReadOnlyCoreDatabase()
    {
        if (database_) {
            sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(database_);
        }
    }

    ReadOnlyCoreDatabase(const ReadOnlyCoreDatabase&) = delete;
    ReadOnlyCoreDatabase& operator=(const ReadOnlyCoreDatabase&) = delete;

    [[nodiscard]] sqlite3* get() const noexcept { return database_; }

private:
    static bool wal_has_frames_with_readable_shm(const std::string& database_path)
    {
        const fs::path wal_path = fs::path(database_path).concat("-wal");
        struct stat wal {};
        if (::lstat(wal_path.c_str(), &wal) != 0) {
            if (errno == ENOENT) return false;
            throw std::runtime_error("cannot inspect Core state WAL");
        }
        if (!S_ISREG(wal.st_mode) || S_ISLNK(wal.st_mode)
            || wal.st_uid != ::geteuid()) {
            throw std::runtime_error("Core state WAL identity is invalid");
        }
        if (wal.st_size == 0) return false;

        const fs::path shm_path = fs::path(database_path).concat("-shm");
        struct stat shm {};
        if (::lstat(shm_path.c_str(), &shm) != 0
            || !S_ISREG(shm.st_mode) || S_ISLNK(shm.st_mode)
            || shm.st_uid != ::geteuid() || shm.st_size == 0) {
            throw std::runtime_error(
                "Core state WAL has frames but readable owned SHM is unavailable");
        }
        return true;
    }

    static std::string readonly_uri(const std::string& path, const bool immutable)
    {
        if (path.find_first_of("%?#") != std::string::npos)
            throw std::runtime_error("Core state database path is not URI-safe");
        return "file:" + path + (immutable ? "?mode=ro&immutable=1" : "?mode=ro");
    }

    sqlite3* database_ = nullptr;
};

std::int64_t scalar(sqlite3* database, const char* sql)
{
    Statement statement(database, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

TimePoint time_point(const std::int64_t value)
{
    return TimePoint{std::chrono::milliseconds{value}};
}

void bind_text(sqlite3* database, sqlite3_stmt* statement,
               const int index, const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}

Task read_task(sqlite3_stmt* statement)
{
    Task task;
    task.id = text(statement, 0);
    task.idempotency_key = text(statement, 1);
    task.kind = text(statement, 2);
    task.input_content_type = text(statement, 3);
    task.input = text(statement, 4);
    task.limits.max_input_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 5));
    task.limits.max_output_bytes = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 6));
    task.limits.max_runtime = std::chrono::milliseconds{
        sqlite3_column_int64(statement, 7)};
    task.limits.max_attempts = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement, 8));
    task.attempts_started = static_cast<std::uint32_t>(
        sqlite3_column_int64(statement, 9));
    task.status = static_cast<TaskStatus>(sqlite3_column_int(statement, 10));
    if (sqlite3_column_type(statement, 11) != SQLITE_NULL) {
        task.lease = gaudere::work::Lease{text(statement, 11),
            time_point(sqlite3_column_int64(statement, 12))};
    }
    task.cancel_reason = text(statement, 13);
    if (sqlite3_column_type(statement, 14) != SQLITE_NULL) {
        task.result = TaskResult{text(statement, 14), text(statement, 15),
                                 text(statement, 16), text(statement, 17),
                                 text(statement, 18), text(statement, 19)};
    }
    return task;
}

std::optional<Task> find_task(sqlite3* database, const std::string& id)
{
    static constexpr const char* sql =
        "SELECT id,idempotency_key,kind,input_content_type,input,"
        "max_input_bytes,max_output_bytes,max_runtime_ms,max_attempts,"
        "attempts_started,status,lease_owner,lease_expires_at_ms,cancel_reason,"
        "result_content_type,result_output,result_failure_code,result_failure_message,"
        "result_metadata_content_type,result_metadata FROM tasks WHERE id=?1";
    Statement statement(database, sql);
    bind_text(database, statement.get(), 1, id);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto task = read_task(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE)
        throw std::runtime_error("local observation Task identity is ambiguous");
    return task;
}

const char* cursor_state_name(const LocalActivityPulseState state) noexcept
{
    switch (state) {
    case LocalActivityPulseState::idle: return "idle";
    case LocalActivityPulseState::preparing: return "preparing";
    case LocalActivityPulseState::settled: return "settled";
    case LocalActivityPulseState::blocked: return "blocked";
    case LocalActivityPulseState::quiescent: return "quiescent";
    }
    return "unknown";
}

const char* task_status_name(const TaskStatus status) noexcept
{
    switch (status) {
    case TaskStatus::pending: return "pending";
    case TaskStatus::running: return "running";
    case TaskStatus::cancel_requested: return "cancel_requested";
    case TaskStatus::succeeded: return "succeeded";
    case TaskStatus::failed: return "failed";
    case TaskStatus::cancelled: return "cancelled";
    case TaskStatus::manual_review: return "manual_review";
    }
    return "unknown";
}

Json nullable_string(const std::optional<std::string>& value)
{
    return value ? Json(*value) : Json(nullptr);
}

Json nullable_int(const std::optional<std::int64_t>& value)
{
    return value ? Json(*value) : Json(nullptr);
}

bool task_matches_cursor(const Task& task,
                         const LocalActivityPulseCursor& cursor,
                         LocalContinuityObservationInspection& observation,
                         std::string& detail) noexcept
{
    observation = inspect_local_continuity_observation_task(task);
    if (!observation.eligible) {
        detail = "cursor Task is not canonical: " + observation.detail;
        return false;
    }
    const auto& facts = observation.facts;
    if (facts.generation != cursor.generation
        || facts.due_at_ms != cursor.due_at_ms
        || !cursor.captured_at_ms
        || facts.captured_at_ms != *cursor.captured_at_ms
        || facts.anchor_checkpoint_task_id != cursor.anchor_checkpoint_task_id
        || facts.anchor_checkpoint_result_sha256
            != cursor.anchor_checkpoint_result_sha256
        || facts.predecessor_observation_task_id
            != cursor.predecessor_observation_task_id
        || facts.predecessor_observation_result_sha256
            != cursor.predecessor_observation_result_sha256) {
        detail = "cursor Task canonical facts do not match durable cursor identity";
        return false;
    }
    return true;
}

LocalActivityPulseStatusInspection unseeded_status(const bool enabled)
{
    const Json status{
        {"anchor_checkpoint_result_sha256", nullptr},
        {"anchor_checkpoint_task_id", nullptr},
        {"blocked_reason", ""},
        {"captured_at_ms", nullptr},
        {"cursor_state", "unseeded"},
        {"due_at_ms", nullptr},
        {"enabled", enabled},
        {"generation", 0},
        {"latest_payload", nullptr},
        {"next_deadline_ms", nullptr},
        {"predecessor_observation_result_sha256", nullptr},
        {"predecessor_observation_task_id", nullptr},
        {"result_sha256", nullptr},
        {"revision", 0},
        {"scheduler_active", false},
        {"schema", local_activity_pulse_status_schema},
        {"seeded", false},
        {"task_id", nullptr},
        {"task_status", nullptr}
    };
    return {true, status.dump(), {}};
}

} // namespace

LocalActivityPulseStatusInspection inspect_local_activity_pulse_status(
    const std::string& state_path,
    const std::string& sidecar_path,
    const bool enabled_source_intent) noexcept
{
    try {
        struct stat sidecar_metadata {};
        if (::lstat(sidecar_path.c_str(), &sidecar_metadata) != 0) {
            if (errno == ENOENT) return unseeded_status(enabled_source_intent);
            return {false, {}, "cannot inspect local activity sidecar path"};
        }

        const auto sidecar = inspect_local_activity_pulse_sidecar(sidecar_path);
        if (!sidecar.eligible || !sidecar.cursor)
            return {false, {}, sidecar.detail.empty()
                ? "local activity sidecar inspection failed" : sidecar.detail};
        const auto& cursor = *sidecar.cursor;

        const auto deadline = inspect_local_activity_pulse_deadline(
            sidecar.cursor, enabled_source_intent);
        if (!deadline.eligible)
            return {false, {}, deadline.detail.empty()
                ? "local activity deadline inspection failed" : deadline.detail};

        ReadOnlyCoreDatabase database(state_path);
        if (scalar(database.get(), "PRAGMA user_version") != 4)
            return {false, {}, "local activity status requires Core SQLite schema v4"};

        std::optional<Task> task;
        std::optional<LocalContinuityObservationInspection> observation;
        if (!cursor.task_id.empty()) {
            task = find_task(database.get(), cursor.task_id);
            if (task) {
                LocalContinuityObservationInspection inspected;
                std::string detail;
                if (!task_matches_cursor(*task, cursor, inspected, detail))
                    return {false, {}, detail};
                observation = std::move(inspected);

                if (task->status == TaskStatus::succeeded) {
                    if (!canonical_local_continuity_observation_success(*task))
                        return {false, {}, "local observation succeeded with non-canonical result"};
                    const auto result_hash = sha256_hex(task->result->output);
                    if (cursor.result_sha256 && result_hash != *cursor.result_sha256)
                        return {false, {}, "cursor result hash differs from succeeded Task result"};
                } else if (task->status == TaskStatus::failed
                           || task->status == TaskStatus::cancelled
                           || task->status == TaskStatus::manual_review) {
                    return {false, {}, "local observation Task is terminal without canonical success"};
                }
            }
        }

        if ((cursor.state == LocalActivityPulseState::settled
             || cursor.state == LocalActivityPulseState::quiescent)
            && (!task || task->status != TaskStatus::succeeded
                || !cursor.result_sha256)) {
            return {false, {},
                "settled or quiescent cursor lacks its canonical succeeded Task result"};
        }
        if (cursor.state == LocalActivityPulseState::preparing
            && cursor.task_id.empty()) {
            return {false, {}, "preparing cursor lacks reserved Task identity"};
        }
        if (cursor.state == LocalActivityPulseState::idle && task)
            return {false, {}, "idle cursor unexpectedly references a Task"};

        Json latest_payload = nullptr;
        if (observation)
            latest_payload = Json::parse(observation->canonical_payload);

        Json next_deadline = nullptr;
        if (deadline.active && deadline.deadline) {
            next_deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline.deadline->time_since_epoch()).count();
        }

        Json status{
            {"anchor_checkpoint_result_sha256", cursor.anchor_checkpoint_result_sha256},
            {"anchor_checkpoint_task_id", cursor.anchor_checkpoint_task_id},
            {"blocked_reason", cursor.blocked_reason},
            {"captured_at_ms", nullable_int(cursor.captured_at_ms)},
            {"cursor_state", cursor_state_name(cursor.state)},
            {"due_at_ms", cursor.due_at_ms},
            {"enabled", enabled_source_intent},
            {"generation", cursor.generation},
            {"latest_payload", latest_payload},
            {"next_deadline_ms", next_deadline},
            {"predecessor_observation_result_sha256",
             nullable_string(cursor.predecessor_observation_result_sha256)},
            {"predecessor_observation_task_id",
             nullable_string(cursor.predecessor_observation_task_id)},
            {"result_sha256", nullable_string(cursor.result_sha256)},
            {"revision", cursor.revision},
            {"scheduler_active", deadline.active},
            {"schema", local_activity_pulse_status_schema},
            {"seeded", true},
            {"task_id", cursor.task_id.empty() ? Json(nullptr) : Json(cursor.task_id)},
            {"task_status", task ? Json(task_status_name(task->status)) : Json(nullptr)}
        };
        return {true, status.dump(), {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    } catch (...) {
        return {false, {}, "local activity status inspection failed"};
    }
}

} // namespace gaudere_agent
