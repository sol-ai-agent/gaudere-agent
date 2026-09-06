#include "LocalActivityPulseStore.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

class ReadOnlyDatabase {
public:
    explicit ReadOnlyDatabase(const std::string& path)
    {
        struct stat status {};
        if (lstat(path.c_str(), &status) != 0
            || !S_ISREG(status.st_mode)
            || status.st_uid != geteuid()
            || (status.st_mode & 0777) != 0600) {
            throw std::runtime_error(
                "local activity sidecar must pre-exist as current-user mode 0600 regular file");
        }
        if (sqlite3_open_v2(path.c_str(), &database_,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database_ ? sqlite3_errmsg(database_)
                                                   : "cannot open local activity sidecar read-only";
            sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
        if (sqlite3_exec(database_, "PRAGMA query_only=ON;",
                         nullptr, nullptr, nullptr) != SQLITE_OK) {
            const std::string message = sqlite3_errmsg(database_);
            sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
    }
    ~ReadOnlyDatabase() { sqlite3_close(database_); }
    ReadOnlyDatabase(const ReadOnlyDatabase&) = delete;
    ReadOnlyDatabase& operator=(const ReadOnlyDatabase&) = delete;
    [[nodiscard]] sqlite3* get() const noexcept { return database_; }
private:
    sqlite3* database_ = nullptr;
};

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

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

std::optional<std::string> optional_text(sqlite3_stmt* statement, const int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    return text(statement, column);
}

std::int64_t scalar(sqlite3* database, const char* sql)
{
    Statement statement(database, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

} // namespace

LocalActivityPulseSidecarInspection
inspect_local_activity_pulse_sidecar(const std::string& path) noexcept
{
    try {
        ReadOnlyDatabase database(path);
        if (scalar(database.get(), "PRAGMA user_version")
            != local_activity_pulse_sidecar_schema) {
            return {false, {}, "local activity sidecar schema is not canonical v1"};
        }
        if (scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%'") != 1
            || scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='local_activity_pulse_cursor'") != 1) {
            return {false, {}, "local activity sidecar table set is not canonical"};
        }
        const auto rows = scalar(database.get(),
            "SELECT COUNT(*) FROM local_activity_pulse_cursor");
        if (rows != 1) {
            return {false, {}, rows == 0
                ? "local activity sidecar is unseeded"
                : "local activity sidecar cursor is ambiguous"};
        }

        Statement statement(database.get(),
            "SELECT scope,revision,generation,state,anchor_checkpoint_task_id,"
            "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
            "task_id,result_sha256,predecessor_observation_task_id,"
            "predecessor_observation_result_sha256,blocked_reason "
            "FROM local_activity_pulse_cursor LIMIT 1");
        if (sqlite3_step(statement.get()) != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(database.get()));

        const auto revision = sqlite3_column_int64(statement.get(), 1);
        const auto generation = sqlite3_column_int64(statement.get(), 2);
        const auto state = sqlite3_column_int(statement.get(), 3);
        if (revision < 0 || generation < 0 || state < 0 || state > 4)
            return {false, {}, "local activity cursor numeric fields are invalid"};

        LocalActivityPulseCursor cursor;
        cursor.scope = text(statement.get(), 0);
        cursor.revision = static_cast<std::uint64_t>(revision);
        cursor.generation = static_cast<std::uint64_t>(generation);
        cursor.state = static_cast<LocalActivityPulseState>(state);
        cursor.anchor_checkpoint_task_id = text(statement.get(), 4);
        cursor.anchor_checkpoint_result_sha256 = text(statement.get(), 5);
        cursor.anchor_at_ms = sqlite3_column_int64(statement.get(), 6);
        cursor.due_at_ms = sqlite3_column_int64(statement.get(), 7);
        if (sqlite3_column_type(statement.get(), 8) != SQLITE_NULL)
            cursor.captured_at_ms = sqlite3_column_int64(statement.get(), 8);
        cursor.task_id = text(statement.get(), 9);
        cursor.result_sha256 = optional_text(statement.get(), 10);
        cursor.predecessor_observation_task_id = optional_text(statement.get(), 11);
        cursor.predecessor_observation_result_sha256 = optional_text(statement.get(), 12);
        cursor.blocked_reason = text(statement.get(), 13);

        if (!valid_local_activity_pulse_cursor(cursor))
            return {false, cursor, "local activity sidecar cursor is non-canonical"};
        return {true, cursor, {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    }
}

} // namespace gaudere_agent
