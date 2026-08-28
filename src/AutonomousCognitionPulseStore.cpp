#include "AutonomousCognitionPulseStore.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

constexpr std::size_t max_task_id_bytes = 1024;
constexpr std::size_t max_blocked_reason_bytes = 1024;

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

void execute(sqlite3* database, const char* sql)
{
    char* error = nullptr;
    if (sqlite3_exec(database, sql, nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(database);
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

void bind_text(sqlite3* database,
               sqlite3_stmt* statement,
               const int index,
               const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}

void bind_optional_int64(sqlite3* database,
                         sqlite3_stmt* statement,
                         const int index,
                         const std::optional<std::int64_t> value)
{
    const auto result = value
        ? sqlite3_bind_int64(statement, index, *value)
        : sqlite3_bind_null(statement, index);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(database));
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

bool safe_text(const std::string& value) noexcept
{
    for (const unsigned char character : value)
        if (character < 0x20u || character == 0x7fu) return false;
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) return false;
    }
    return true;
}

bool same_cursor(const AutonomousCognitionPulseCursor& left,
                 const AutonomousCognitionPulseCursor& right) noexcept
{
    return left.scope == right.scope
        && left.revision == right.revision
        && left.generation == right.generation
        && left.state == right.state
        && left.predecessor_task_id == right.predecessor_task_id
        && left.predecessor_result_sha256 == right.predecessor_result_sha256
        && left.anchor_at_ms == right.anchor_at_ms
        && left.due_at_ms == right.due_at_ms
        && left.observed_at_ms == right.observed_at_ms
        && left.snapshot_task_id == right.snapshot_task_id
        && left.current_task_id == right.current_task_id
        && left.blocked_reason == right.blocked_reason;
}

AutonomousCognitionPulseCursor read_cursor(sqlite3_stmt* statement)
{
    const auto revision = sqlite3_column_int64(statement, 1);
    const auto generation = sqlite3_column_int64(statement, 2);
    const auto state_value = sqlite3_column_int(statement, 3);
    if (revision < 0 || generation < 0 || state_value < 0 || state_value > 4)
        throw std::runtime_error("invalid autonomous cognition pulse cursor row");

    AutonomousCognitionPulseCursor cursor;
    cursor.scope = text(statement, 0);
    cursor.revision = static_cast<std::uint64_t>(revision);
    cursor.generation = static_cast<std::uint64_t>(generation);
    cursor.state = static_cast<AutonomousCognitionPulseState>(state_value);
    cursor.predecessor_task_id = text(statement, 4);
    cursor.predecessor_result_sha256 = text(statement, 5);
    cursor.anchor_at_ms = sqlite3_column_int64(statement, 6);
    cursor.due_at_ms = sqlite3_column_int64(statement, 7);
    if (sqlite3_column_type(statement, 8) != SQLITE_NULL)
        cursor.observed_at_ms = sqlite3_column_int64(statement, 8);
    cursor.snapshot_task_id = text(statement, 9);
    cursor.current_task_id = text(statement, 10);
    cursor.blocked_reason = text(statement, 11);
    if (!valid_autonomous_cognition_pulse_cursor(cursor))
        throw std::runtime_error("non-canonical autonomous cognition pulse cursor row");
    return cursor;
}

constexpr const char* cursor_columns =
    "scope,revision,generation,state,predecessor_task_id,"
    "predecessor_result_sha256,anchor_at_ms,due_at_ms,observed_at_ms,"
    "snapshot_task_id,current_task_id,blocked_reason";

std::int64_t user_table_count(sqlite3* database)
{
    Statement statement(database,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

void bind_cursor_values(sqlite3* database,
                        sqlite3_stmt* statement,
                        const AutonomousCognitionPulseCursor& cursor,
                        const int first_index)
{
    int index = first_index;
    if (sqlite3_bind_int64(statement, index++,
            static_cast<sqlite3_int64>(cursor.revision)) != SQLITE_OK
        || sqlite3_bind_int64(statement, index++,
            static_cast<sqlite3_int64>(cursor.generation)) != SQLITE_OK
        || sqlite3_bind_int(statement, index++,
            static_cast<int>(cursor.state)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    bind_text(database, statement, index++, cursor.predecessor_task_id);
    bind_text(database, statement, index++, cursor.predecessor_result_sha256);
    if (sqlite3_bind_int64(statement, index++, cursor.anchor_at_ms) != SQLITE_OK
        || sqlite3_bind_int64(statement, index++, cursor.due_at_ms) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
    bind_optional_int64(database, statement, index++, cursor.observed_at_ms);
    bind_text(database, statement, index++, cursor.snapshot_task_id);
    bind_text(database, statement, index++, cursor.current_task_id);
    bind_text(database, statement, index++, cursor.blocked_reason);
}

} // namespace

bool valid_autonomous_cognition_pulse_cursor(
    const AutonomousCognitionPulseCursor& cursor) noexcept
{
    if (cursor.scope != autonomous_cognition_pulse_scope
        || cursor.revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || cursor.generation > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || cursor.predecessor_task_id.empty()
        || cursor.predecessor_task_id.size() > max_task_id_bytes
        || !safe_text(cursor.predecessor_task_id)
        || !lowercase_sha256(cursor.predecessor_result_sha256)
        || cursor.anchor_at_ms < 0
        || cursor.due_at_ms < cursor.anchor_at_ms
        || (cursor.observed_at_ms && *cursor.observed_at_ms < 0)
        || cursor.snapshot_task_id.size() > max_task_id_bytes
        || cursor.current_task_id.size() > max_task_id_bytes
        || (!cursor.snapshot_task_id.empty() && !safe_text(cursor.snapshot_task_id))
        || (!cursor.current_task_id.empty() && !safe_text(cursor.current_task_id))
        || cursor.blocked_reason.size() > max_blocked_reason_bytes
        || (!cursor.blocked_reason.empty() && !safe_text(cursor.blocked_reason))) {
        return false;
    }

    switch (cursor.state) {
    case AutonomousCognitionPulseState::idle:
    case AutonomousCognitionPulseState::quiescent:
        return !cursor.observed_at_ms
            && cursor.snapshot_task_id.empty()
            && cursor.current_task_id.empty()
            && cursor.blocked_reason.empty();
    case AutonomousCognitionPulseState::preparing:
        return cursor.observed_at_ms
            && cursor.snapshot_task_id.empty()
            && cursor.current_task_id.empty()
            && cursor.blocked_reason.empty();
    case AutonomousCognitionPulseState::prepared:
        return cursor.observed_at_ms
            && !cursor.snapshot_task_id.empty()
            && !cursor.current_task_id.empty()
            && cursor.blocked_reason.empty();
    case AutonomousCognitionPulseState::blocked:
        return !cursor.blocked_reason.empty();
    }
    return false;
}

AutonomousCognitionPulseStore::AutonomousCognitionPulseStore(
    const std::string& path)
{
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE
                            | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_)
                                               : "cannot open pulse sidecar SQLite";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }

    try {
        execute(database_, "PRAGMA journal_mode=WAL;");
        execute(database_, "PRAGMA synchronous=FULL;");
        execute(database_, "PRAGMA busy_timeout=5000;");
        execute(database_, "BEGIN IMMEDIATE;");

        Statement version_statement(database_, "PRAGMA user_version");
        if (sqlite3_step(version_statement.get()) != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(database_));
        const int version = sqlite3_column_int(version_statement.get(), 0);
        if (version > autonomous_cognition_pulse_sidecar_schema)
            throw std::runtime_error("unsupported cognition pulse sidecar schema");

        if (version == 0) {
            if (user_table_count(database_) != 0)
                throw std::runtime_error(
                    "unversioned cognition pulse sidecar is not empty");
            execute(database_,
                "CREATE TABLE autonomous_cognition_pulse_cursor ("
                " scope TEXT PRIMARY KEY NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " generation INTEGER NOT NULL CHECK(generation >= 0),"
                " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
                " predecessor_task_id TEXT NOT NULL,"
                " predecessor_result_sha256 TEXT NOT NULL,"
                " anchor_at_ms INTEGER NOT NULL CHECK(anchor_at_ms >= 0),"
                " due_at_ms INTEGER NOT NULL CHECK(due_at_ms >= anchor_at_ms),"
                " observed_at_ms INTEGER CHECK(observed_at_ms IS NULL OR observed_at_ms >= 0),"
                " snapshot_task_id TEXT NOT NULL,"
                " current_task_id TEXT NOT NULL,"
                " blocked_reason TEXT NOT NULL"
                ");");
            execute(database_, "PRAGMA user_version=1;");
        } else {
            Statement schema_probe(database_,
                "SELECT scope,revision,generation,state,predecessor_task_id,"
                "predecessor_result_sha256,anchor_at_ms,due_at_ms,observed_at_ms,"
                "snapshot_task_id,current_task_id,blocked_reason "
                "FROM autonomous_cognition_pulse_cursor LIMIT 0");
            (void)schema_probe;
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

AutonomousCognitionPulseStore::~AutonomousCognitionPulseStore()
{
    sqlite3_close(database_);
}

std::optional<AutonomousCognitionPulseCursor>
AutonomousCognitionPulseStore::find(const std::string& scope) const
{
    const std::string sql = std::string{"SELECT "} + cursor_columns
        + " FROM autonomous_cognition_pulse_cursor WHERE scope=?1 LIMIT 1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, scope);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return read_cursor(statement.get());
}

AutonomousCognitionPulseStoreWrite AutonomousCognitionPulseStore::seed(
    const AutonomousCognitionPulseCursor& cursor)
{
    if (!valid_autonomous_cognition_pulse_cursor(cursor) || cursor.revision != 0)
        return {AutonomousCognitionPulseStoreResult::invalid, {},
                "pulse seed cursor is non-canonical"};
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = find(cursor.scope);
        if (existing) {
            execute(database_, "ROLLBACK;");
            return same_cursor(*existing, cursor)
                ? AutonomousCognitionPulseStoreWrite{
                    AutonomousCognitionPulseStoreResult::duplicate, existing, {}}
                : AutonomousCognitionPulseStoreWrite{
                    AutonomousCognitionPulseStoreResult::conflict, existing,
                    "pulse sidecar is already seeded differently"};
        }

        Statement statement(database_,
            "INSERT INTO autonomous_cognition_pulse_cursor ("
            "scope,revision,generation,state,predecessor_task_id,"
            "predecessor_result_sha256,anchor_at_ms,due_at_ms,observed_at_ms,"
            "snapshot_task_id,current_task_id,blocked_reason) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
        bind_text(database_, statement.get(), 1, cursor.scope);
        bind_cursor_values(database_, statement.get(), cursor, 2);
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));
        execute(database_, "COMMIT;");
        const auto stored = find(cursor.scope);
        if (!stored || !same_cursor(*stored, cursor))
            return {AutonomousCognitionPulseStoreResult::conflict, stored,
                    "pulse seed did not persist exact cursor"};
        return {AutonomousCognitionPulseStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {AutonomousCognitionPulseStoreResult::unavailable, {}, error.what()};
    }
}

AutonomousCognitionPulseStoreWrite AutonomousCognitionPulseStore::replace(
    const AutonomousCognitionPulseCursor& expected,
    const AutonomousCognitionPulseCursor& replacement)
{
    if (!valid_autonomous_cognition_pulse_cursor(expected)
        || !valid_autonomous_cognition_pulse_cursor(replacement)
        || expected.scope != replacement.scope
        || expected.revision == static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || replacement.revision != expected.revision + 1) {
        return {AutonomousCognitionPulseStoreResult::invalid, {},
                "pulse cursor replacement is non-canonical"};
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        Statement statement(database_,
            "UPDATE autonomous_cognition_pulse_cursor SET "
            "revision=?1,generation=?2,state=?3,predecessor_task_id=?4,"
            "predecessor_result_sha256=?5,anchor_at_ms=?6,due_at_ms=?7,"
            "observed_at_ms=?8,snapshot_task_id=?9,current_task_id=?10,"
            "blocked_reason=?11 WHERE scope=?12 AND revision=?13");
        bind_cursor_values(database_, statement.get(), replacement, 1);
        bind_text(database_, statement.get(), 12, expected.scope);
        if (sqlite3_bind_int64(statement.get(), 13,
                static_cast<sqlite3_int64>(expected.revision)) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));

        if (sqlite3_changes(database_) != 1) {
            const auto current = find(expected.scope);
            execute(database_, "ROLLBACK;");
            if (current && same_cursor(*current, replacement)) {
                return {AutonomousCognitionPulseStoreResult::duplicate, current, {}};
            }
            return {AutonomousCognitionPulseStoreResult::conflict, current,
                    "pulse cursor revision changed concurrently"};
        }

        execute(database_, "COMMIT;");
        const auto stored = find(expected.scope);
        if (!stored || !same_cursor(*stored, replacement))
            return {AutonomousCognitionPulseStoreResult::conflict, stored,
                    "pulse cursor replacement did not persist exactly"};
        return {AutonomousCognitionPulseStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {AutonomousCognitionPulseStoreResult::unavailable, {}, error.what()};
    }
}

} // namespace gaudere_agent
