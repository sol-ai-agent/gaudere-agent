#include "AutonomousCognitionPulseStore.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

class ReadOnlyDatabase {
public:
    explicit ReadOnlyDatabase(const std::string& path)
    {
        if (sqlite3_open_v2(path.c_str(), &database_,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database_ ? sqlite3_errmsg(database_)
                                                  : "cannot open pulse sidecar read-only";
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

std::int64_t scalar(sqlite3* database, const char* sql)
{
    Statement statement(database, sql);
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

} // namespace

AutonomousCognitionPulseSidecarInspection
inspect_autonomous_cognition_pulse_sidecar(const std::string& path) noexcept
{
    try {
        ReadOnlyDatabase database(path);
        if (scalar(database.get(), "PRAGMA user_version")
            != autonomous_cognition_pulse_sidecar_schema) {
            return {false, {}, "pulse sidecar schema is not canonical v1"};
        }
        if (scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name NOT LIKE 'sqlite_%'") != 1
            || scalar(database.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                "AND name='autonomous_cognition_pulse_cursor'") != 1) {
            return {false, {}, "pulse sidecar table set is not canonical"};
        }
        const auto rows = scalar(database.get(),
            "SELECT COUNT(*) FROM autonomous_cognition_pulse_cursor");
        if (rows != 1) {
            return {false, {},
                    rows == 0 ? "pulse sidecar is unseeded"
                              : "pulse sidecar cursor is ambiguous"};
        }

        Statement statement(database.get(),
            "SELECT scope,revision,generation,state,predecessor_task_id,"
            "predecessor_result_sha256,anchor_at_ms,due_at_ms,observed_at_ms,"
            "snapshot_task_id,current_task_id,blocked_reason "
            "FROM autonomous_cognition_pulse_cursor LIMIT 1");
        if (sqlite3_step(statement.get()) != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(database.get()));

        const auto revision = sqlite3_column_int64(statement.get(), 1);
        const auto generation = sqlite3_column_int64(statement.get(), 2);
        const auto state = sqlite3_column_int(statement.get(), 3);
        if (revision < 0 || generation < 0 || state < 0 || state > 4) {
            return {false, {}, "pulse sidecar cursor numeric fields are invalid"};
        }

        AutonomousCognitionPulseCursor cursor;
        cursor.scope = text(statement.get(), 0);
        cursor.revision = static_cast<std::uint64_t>(revision);
        cursor.generation = static_cast<std::uint64_t>(generation);
        cursor.state = static_cast<AutonomousCognitionPulseState>(state);
        cursor.predecessor_task_id = text(statement.get(), 4);
        cursor.predecessor_result_sha256 = text(statement.get(), 5);
        cursor.anchor_at_ms = sqlite3_column_int64(statement.get(), 6);
        cursor.due_at_ms = sqlite3_column_int64(statement.get(), 7);
        if (sqlite3_column_type(statement.get(), 8) != SQLITE_NULL)
            cursor.observed_at_ms = sqlite3_column_int64(statement.get(), 8);
        cursor.snapshot_task_id = text(statement.get(), 9);
        cursor.current_task_id = text(statement.get(), 10);
        cursor.blocked_reason = text(statement.get(), 11);
        if (!valid_autonomous_cognition_pulse_cursor(cursor)) {
            return {false, cursor, "pulse sidecar cursor is non-canonical"};
        }
        return {true, cursor, {}};
    } catch (const std::exception& error) {
        return {false, {}, error.what()};
    }
}

} // namespace gaudere_agent
