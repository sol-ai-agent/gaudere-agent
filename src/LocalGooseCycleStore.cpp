#include "LocalGooseCycleStore.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

constexpr std::size_t max_id_bytes = 1024;
constexpr std::size_t max_blocked_reason_bytes = 1024;
constexpr const char* observation_prefix = "continuity.local-observation.v1:";
constexpr const char* cycle_prefix = "cognition.local-goose-cycle.v1:";

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

bool prefixed_sha256(const std::string& value, const char* prefix) noexcept
{
    const std::string prefix_string{prefix};
    return value.size() == prefix_string.size() + 64
        && value.compare(0, prefix_string.size(), prefix_string) == 0
        && lowercase_sha256(value.substr(prefix_string.size()));
}

bool predecessor_shape(const LocalGooseCycleCursor& cursor) noexcept
{
    if (cursor.generation == 0)
        return !cursor.predecessor_task_id && !cursor.predecessor_result_sha256;
    if (!cursor.predecessor_task_id && !cursor.predecessor_result_sha256)
        return cursor.generation == 1;
    return cursor.predecessor_task_id && cursor.predecessor_result_sha256
        && prefixed_sha256(*cursor.predecessor_task_id, cycle_prefix)
        && lowercase_sha256(*cursor.predecessor_result_sha256);
}

bool same_cursor(const LocalGooseCycleCursor& left,
                 const LocalGooseCycleCursor& right) noexcept
{
    return left.scope == right.scope
        && left.revision == right.revision
        && left.generation == right.generation
        && left.state == right.state
        && left.anchor_observation_task_id == right.anchor_observation_task_id
        && left.anchor_observation_result_sha256
            == right.anchor_observation_result_sha256
        && left.predecessor_task_id == right.predecessor_task_id
        && left.predecessor_result_sha256 == right.predecessor_result_sha256
        && left.due_at_ms == right.due_at_ms
        && left.captured_at_ms == right.captured_at_ms
        && left.current_task_id == right.current_task_id
        && left.blocked_reason == right.blocked_reason;
}

bool immutable_anchor(const LocalGooseCycleCursor& left,
                      const LocalGooseCycleCursor& right) noexcept
{
    return left.scope == right.scope
        && left.anchor_observation_task_id == right.anchor_observation_task_id
        && left.anchor_observation_result_sha256
            == right.anchor_observation_result_sha256;
}

bool same_predecessor(const LocalGooseCycleCursor& left,
                      const LocalGooseCycleCursor& right) noexcept
{
    return left.predecessor_task_id == right.predecessor_task_id
        && left.predecessor_result_sha256 == right.predecessor_result_sha256;
}

void bind_text(sqlite3* database, sqlite3_stmt* statement,
               const int index, const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}

void bind_optional_text(sqlite3* database, sqlite3_stmt* statement,
                        const int index, const std::optional<std::string>& value)
{
    const int result = value
        ? sqlite3_bind_text64(statement, index, value->data(), value->size(),
                              SQLITE_TRANSIENT, SQLITE_UTF8)
        : sqlite3_bind_null(statement, index);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(database));
}

void bind_optional_int64(sqlite3* database, sqlite3_stmt* statement,
                         const int index, const std::optional<std::int64_t> value)
{
    const int result = value
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

LocalGooseCycleCursor read_cursor(sqlite3_stmt* statement)
{
    const auto revision = sqlite3_column_int64(statement, 1);
    const auto generation = sqlite3_column_int64(statement, 2);
    const auto state = sqlite3_column_int(statement, 3);
    if (revision < 0 || generation < 0 || state < 0 || state > 3)
        throw std::runtime_error("invalid Local Goose cycle cursor row");

    LocalGooseCycleCursor cursor;
    cursor.scope = text(statement, 0);
    cursor.revision = static_cast<std::uint64_t>(revision);
    cursor.generation = static_cast<std::uint64_t>(generation);
    cursor.state = static_cast<LocalGooseCycleState>(state);
    cursor.anchor_observation_task_id = text(statement, 4);
    cursor.anchor_observation_result_sha256 = text(statement, 5);
    cursor.predecessor_task_id = optional_text(statement, 6);
    cursor.predecessor_result_sha256 = optional_text(statement, 7);
    cursor.due_at_ms = optional_int64(statement, 8);
    cursor.captured_at_ms = optional_int64(statement, 9);
    cursor.current_task_id = text(statement, 10);
    cursor.blocked_reason = text(statement, 11);
    if (!valid_local_goose_cycle_cursor(cursor))
        throw std::runtime_error("non-canonical Local Goose cycle cursor row");
    return cursor;
}

constexpr const char* cursor_columns =
    "scope,revision,generation,state,anchor_observation_task_id,"
    "anchor_observation_result_sha256,predecessor_task_id,"
    "predecessor_result_sha256,due_at_ms,captured_at_ms,current_task_id,"
    "blocked_reason";

std::int64_t user_table_count(sqlite3* database)
{
    Statement statement(database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%'");
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

void bind_cursor_values(sqlite3* database, sqlite3_stmt* statement,
                        const LocalGooseCycleCursor& cursor, int index)
{
    if (sqlite3_bind_int64(statement, index++,
            static_cast<sqlite3_int64>(cursor.revision)) != SQLITE_OK
        || sqlite3_bind_int64(statement, index++,
            static_cast<sqlite3_int64>(cursor.generation)) != SQLITE_OK
        || sqlite3_bind_int(statement, index++,
            static_cast<int>(cursor.state)) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
    bind_text(database, statement, index++, cursor.anchor_observation_task_id);
    bind_text(database, statement, index++, cursor.anchor_observation_result_sha256);
    bind_optional_text(database, statement, index++, cursor.predecessor_task_id);
    bind_optional_text(database, statement, index++, cursor.predecessor_result_sha256);
    bind_optional_int64(database, statement, index++, cursor.due_at_ms);
    bind_optional_int64(database, statement, index++, cursor.captured_at_ms);
    bind_text(database, statement, index++, cursor.current_task_id);
    bind_text(database, statement, index++, cursor.blocked_reason);
}

void verify_owner_mode(const std::string& path)
{
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0)
        throw std::runtime_error("cannot stat Local Goose cycle sidecar");
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600)
        throw std::runtime_error(
            "Local Goose cycle sidecar must be a current-user regular file mode 0600");
}

void ensure_secure_file(const std::string& path)
{
    const int descriptor = open(path.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
        close(descriptor);
        verify_owner_mode(path);
        return;
    }
    if (errno != EEXIST)
        throw std::runtime_error("cannot securely create Local Goose cycle sidecar");
    verify_owner_mode(path);
}

} // namespace

bool valid_local_goose_cycle_cursor(const LocalGooseCycleCursor& cursor) noexcept
{
    if (cursor.scope != local_goose_cycle_scope
        || cursor.revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || cursor.generation > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || !prefixed_sha256(cursor.anchor_observation_task_id, observation_prefix)
        || !lowercase_sha256(cursor.anchor_observation_result_sha256)
        || !predecessor_shape(cursor)
        || (cursor.due_at_ms && *cursor.due_at_ms < 0)
        || (cursor.captured_at_ms && *cursor.captured_at_ms < 0)
        || cursor.current_task_id.size() > max_id_bytes
        || (!cursor.current_task_id.empty() && !safe_text(cursor.current_task_id))
        || cursor.blocked_reason.size() > max_blocked_reason_bytes
        || (!cursor.blocked_reason.empty() && !safe_text(cursor.blocked_reason)))
        return false;

    switch (cursor.state) {
    case LocalGooseCycleState::dormant:
        return !cursor.due_at_ms && !cursor.captured_at_ms
            && cursor.current_task_id.empty() && cursor.blocked_reason.empty();
    case LocalGooseCycleState::scheduled:
        return cursor.generation >= 1 && cursor.due_at_ms
            && !cursor.captured_at_ms && cursor.current_task_id.empty()
            && cursor.blocked_reason.empty();
    case LocalGooseCycleState::prepared:
        return cursor.generation >= 1 && cursor.due_at_ms && cursor.captured_at_ms
            && *cursor.captured_at_ms >= *cursor.due_at_ms
            && prefixed_sha256(cursor.current_task_id, cycle_prefix)
            && cursor.blocked_reason.empty();
    case LocalGooseCycleState::blocked:
        return !cursor.blocked_reason.empty();
    }
    return false;
}

bool valid_local_goose_cycle_transition(
    const LocalGooseCycleCursor& expected,
    const LocalGooseCycleCursor& replacement) noexcept
{
    if (!valid_local_goose_cycle_cursor(expected)
        || !valid_local_goose_cycle_cursor(replacement)
        || !immutable_anchor(expected, replacement)
        || expected.revision == static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || replacement.revision != expected.revision + 1
        || expected.state == LocalGooseCycleState::blocked)
        return false;

    if (replacement.state == LocalGooseCycleState::blocked)
        return replacement.generation == expected.generation;

    switch (expected.state) {
    case LocalGooseCycleState::dormant:
        return replacement.state == LocalGooseCycleState::scheduled
            && replacement.generation == expected.generation + 1
            && same_predecessor(expected, replacement);

    case LocalGooseCycleState::scheduled:
        return replacement.state == LocalGooseCycleState::prepared
            && replacement.generation == expected.generation
            && replacement.due_at_ms == expected.due_at_ms
            && same_predecessor(expected, replacement);

    case LocalGooseCycleState::prepared: {
        const bool predecessor_advanced =
            replacement.predecessor_task_id
                == std::optional<std::string>{expected.current_task_id}
            && replacement.predecessor_result_sha256.has_value();
        if (!predecessor_advanced) return false;
        if (replacement.state == LocalGooseCycleState::dormant)
            return replacement.generation == expected.generation;
        if (replacement.state == LocalGooseCycleState::scheduled)
            return replacement.generation == expected.generation + 1;
        return false;
    }

    case LocalGooseCycleState::blocked:
        return false;
    }
    return false;
}

LocalGooseCycleStore::LocalGooseCycleStore(const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_)
                                               : "cannot open Local Goose cycle sidecar SQLite";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }

    try {
        execute(database_, "PRAGMA journal_mode=DELETE;");
        execute(database_, "PRAGMA synchronous=FULL;");
        execute(database_, "PRAGMA busy_timeout=5000;");
        execute(database_, "BEGIN IMMEDIATE;");

        Statement version_statement(database_, "PRAGMA user_version");
        if (sqlite3_step(version_statement.get()) != SQLITE_ROW)
            throw std::runtime_error(sqlite3_errmsg(database_));
        const int version = sqlite3_column_int(version_statement.get(), 0);
        if (version > local_goose_cycle_sidecar_schema)
            throw std::runtime_error("unsupported Local Goose cycle sidecar schema");

        if (version == 0) {
            if (user_table_count(database_) != 0)
                throw std::runtime_error("unversioned Local Goose cycle sidecar is not empty");
            execute(database_,
                "CREATE TABLE local_goose_cycle_cursor ("
                " scope TEXT PRIMARY KEY NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " generation INTEGER NOT NULL CHECK(generation >= 0),"
                " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),"
                " anchor_observation_task_id TEXT NOT NULL,"
                " anchor_observation_result_sha256 TEXT NOT NULL,"
                " predecessor_task_id TEXT,"
                " predecessor_result_sha256 TEXT,"
                " due_at_ms INTEGER CHECK(due_at_ms IS NULL OR due_at_ms >= 0),"
                " captured_at_ms INTEGER CHECK(captured_at_ms IS NULL OR captured_at_ms >= 0),"
                " current_task_id TEXT NOT NULL,"
                " blocked_reason TEXT NOT NULL"
                ");");
            execute(database_, "PRAGMA user_version=1;");
        } else {
            Statement schema_probe(database_,
                "SELECT scope,revision,generation,state,anchor_observation_task_id,"
                "anchor_observation_result_sha256,predecessor_task_id,"
                "predecessor_result_sha256,due_at_ms,captured_at_ms,current_task_id,"
                "blocked_reason FROM local_goose_cycle_cursor LIMIT 0");
            (void)schema_probe;
        }
        execute(database_, "COMMIT;");
        verify_owner_mode(path);
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseCycleStore::~LocalGooseCycleStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseCycleCursor>
LocalGooseCycleStore::find(const std::string& scope) const
{
    const std::string sql = std::string{"SELECT "} + cursor_columns
        + " FROM local_goose_cycle_cursor WHERE scope=?1 LIMIT 1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, scope);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return read_cursor(statement.get());
}

LocalGooseCycleStoreWrite LocalGooseCycleStore::seed(
    const LocalGooseCycleCursor& cursor)
{
    if (!valid_local_goose_cycle_cursor(cursor)
        || cursor.revision != 0 || cursor.generation != 0
        || cursor.state != LocalGooseCycleState::dormant) {
        return {LocalGooseCycleStoreResult::invalid, {},
                "Local Goose cycle seed cursor is non-canonical"};
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = find(cursor.scope);
        if (existing) {
            execute(database_, "ROLLBACK;");
            return same_cursor(*existing, cursor)
                ? LocalGooseCycleStoreWrite{LocalGooseCycleStoreResult::duplicate,
                                            existing, {}}
                : LocalGooseCycleStoreWrite{LocalGooseCycleStoreResult::conflict,
                                            existing,
                                            "Local Goose cycle sidecar is already seeded differently"};
        }

        Statement statement(database_,
            "INSERT INTO local_goose_cycle_cursor ("
            "scope,revision,generation,state,anchor_observation_task_id,"
            "anchor_observation_result_sha256,predecessor_task_id,"
            "predecessor_result_sha256,due_at_ms,captured_at_ms,current_task_id,"
            "blocked_reason) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
        bind_text(database_, statement.get(), 1, cursor.scope);
        bind_cursor_values(database_, statement.get(), cursor, 2);
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));
        execute(database_, "COMMIT;");
        const auto stored = find(cursor.scope);
        if (!stored || !same_cursor(*stored, cursor))
            return {LocalGooseCycleStoreResult::conflict, stored,
                    "Local Goose cycle seed did not persist exact cursor"};
        return {LocalGooseCycleStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalGooseCycleStoreResult::unavailable, {}, error.what()};
    }
}

LocalGooseCycleStoreWrite LocalGooseCycleStore::replace(
    const LocalGooseCycleCursor& expected,
    const LocalGooseCycleCursor& replacement)
{
    if (!valid_local_goose_cycle_transition(expected, replacement))
        return {LocalGooseCycleStoreResult::invalid, {},
                "Local Goose cycle cursor transition is non-canonical"};

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        Statement statement(database_,
            "UPDATE local_goose_cycle_cursor SET "
            "revision=?1,generation=?2,state=?3,anchor_observation_task_id=?4,"
            "anchor_observation_result_sha256=?5,predecessor_task_id=?6,"
            "predecessor_result_sha256=?7,due_at_ms=?8,captured_at_ms=?9,"
            "current_task_id=?10,blocked_reason=?11 "
            "WHERE scope=?12 AND revision=?13");
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
            if (current && same_cursor(*current, replacement))
                return {LocalGooseCycleStoreResult::duplicate, current, {}};
            return {LocalGooseCycleStoreResult::conflict, current,
                    "Local Goose cycle cursor revision changed concurrently"};
        }

        execute(database_, "COMMIT;");
        const auto stored = find(expected.scope);
        if (!stored || !same_cursor(*stored, replacement))
            return {LocalGooseCycleStoreResult::conflict, stored,
                    "Local Goose cycle cursor replacement did not persist exactly"};
        return {LocalGooseCycleStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalGooseCycleStoreResult::unavailable, {}, error.what()};
    }
}

} // namespace gaudere_agent
