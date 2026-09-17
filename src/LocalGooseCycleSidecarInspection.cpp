#include "LocalGooseCycleStore.hpp"

#include <sqlite3.h>

#include <cstddef>
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
            || (status.st_mode & 0777) != 0600)
            throw std::runtime_error(
                "Local Goose cycle sidecar must pre-exist as current-user mode 0600 regular file");

        if (sqlite3_open_v2(path.c_str(), &database_,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database_ ? sqlite3_errmsg(database_)
                                                   : "cannot open Local Goose cycle sidecar read-only";
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

std::optional<std::int64_t> optional_int64(sqlite3_stmt* statement, const int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_int64(statement, column);
}

std::int64_t scalar(sqlite3* database, const char* sql)
{
    Statement statement(database, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

} // namespace

LocalGooseCycleSidecarInspection
inspect_local_goose_cycle_sidecar(const std::string& path) noexcept
{
    try {
        ReadOnlyDatabase database(path);
        if (scalar(database.get(), "PRAGMA user_version")
            != local_goose_cycle_sidecar_schema)
            return {false, {}, "Local Goose cycle sidecar schema is not canonical v1"};

        if (scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%'") != 1
            || scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='local_goose_cycle_cursor'") != 1)
            return {false, {}, "Local Goose cycle sidecar table set is not canonical"};

        const auto rows = scalar(database.get(),
            "SELECT COUNT(*) FROM local_goose_cycle_cursor");
        if (rows != 1)
            return {false, {}, rows == 0
                ? "Local Goose cycle sidecar is unseeded"
                : "Local Goose cycle sidecar cursor is ambiguous"};

        Statement statement(database.get(),
            "SELECT scope,revision,generation,state,anchor_observation_task_id,"
            "anchor_observation_result_sha256,predecessor_task_id,"
            "predecessor_result_sha256,due_at_ms,captured_at_ms,current_task_id,"
            "blocked_reason FROM local_goose_cycle_cursor LIMIT 1");
        if (sqlite3_step(statement.get()) != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(database.get()));

        const auto revision = sqlite3_column_int64(statement.get(), 1);
        const auto generation = sqlite3_column_int64(statement.get(), 2);
        const auto state = sqlite3_column_int(statement.get(), 3);
        if (revision < 0 || generation < 0 || state < 0 || state > 3)
            return {false, {}, "Local Goose cycle cursor numeric fields are invalid"};

        LocalGooseCycleCursor cursor;
        cursor.scope = text(statement.get(), 0);
        cursor.revision = static_cast<std::uint64_t>(revision);
        cursor.generation = static_cast<std::uint64_t>(generation);
        cursor.state = static_cast<LocalGooseCycleState>(state);
        cursor.anchor_observation_task_id = text(statement.get(), 4);
        cursor.anchor_observation_result_sha256 = text(statement.get(), 5);
        cursor.predecessor_task_id = optional_text(statement.get(), 6);
        cursor.predecessor_result_sha256 = optional_text(statement.get(), 7);
        cursor.due_at_ms = optional_int64(statement.get(), 8);
        cursor.captured_at_ms = optional_int64(statement.get(), 9);
        cursor.current_task_id = text(statement.get(), 10);
        cursor.blocked_reason = text(statement.get(), 11);

        if (!valid_local_goose_cycle_cursor(cursor))
            return {false, cursor, "Local Goose cycle sidecar cursor is non-canonical"};
        return {true, cursor, {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    }
}

} // namespace gaudere_agent
