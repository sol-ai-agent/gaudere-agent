#include "LocalActivityPulseStore.hpp"

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
constexpr const char* checkpoint_prefix = "continuity.delta-checkpoint.v1:";
constexpr const char* observation_prefix = "continuity.local-observation.v1:";

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

bool optional_safe_id(const std::optional<std::string>& value) noexcept
{
    return !value || (!value->empty() && value->size() <= max_id_bytes
        && safe_text(*value));
}

bool optional_sha(const std::optional<std::string>& value) noexcept
{
    return !value || lowercase_sha256(*value);
}

bool predecessor_shape(const LocalActivityPulseCursor& cursor) noexcept
{
    if (cursor.generation <= 1)
        return !cursor.predecessor_observation_task_id
            && !cursor.predecessor_observation_result_sha256;
    return cursor.predecessor_observation_task_id
        && cursor.predecessor_observation_result_sha256
        && prefixed_sha256(*cursor.predecessor_observation_task_id,
                           observation_prefix)
        && lowercase_sha256(*cursor.predecessor_observation_result_sha256);
}

bool active_generation_shape(const LocalActivityPulseCursor& cursor,
                             const bool result_required) noexcept
{
    return cursor.generation >= 1 && cursor.generation <= 3
        && cursor.captured_at_ms
        && *cursor.captured_at_ms >= 0
        && cursor.task_id.size() <= max_id_bytes
        && prefixed_sha256(cursor.task_id, observation_prefix)
        && predecessor_shape(cursor)
        && optional_sha(cursor.result_sha256)
        && (result_required ? cursor.result_sha256.has_value()
                            : !cursor.result_sha256.has_value());
}

bool same_cursor(const LocalActivityPulseCursor& left,
                 const LocalActivityPulseCursor& right) noexcept
{
    return left.scope == right.scope
        && left.revision == right.revision
        && left.generation == right.generation
        && left.state == right.state
        && left.anchor_checkpoint_task_id == right.anchor_checkpoint_task_id
        && left.anchor_checkpoint_result_sha256 == right.anchor_checkpoint_result_sha256
        && left.anchor_at_ms == right.anchor_at_ms
        && left.due_at_ms == right.due_at_ms
        && left.captured_at_ms == right.captured_at_ms
        && left.task_id == right.task_id
        && left.result_sha256 == right.result_sha256
        && left.predecessor_observation_task_id
            == right.predecessor_observation_task_id
        && left.predecessor_observation_result_sha256
            == right.predecessor_observation_result_sha256
        && left.blocked_reason == right.blocked_reason;
}

void bind_text(sqlite3* database, sqlite3_stmt* statement,
               const int index, const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
}

void bind_optional_text(sqlite3* database, sqlite3_stmt* statement,
                        const int index,
                        const std::optional<std::string>& value)
{
    const int result = value
        ? sqlite3_bind_text64(statement, index, value->data(), value->size(),
                              SQLITE_TRANSIENT, SQLITE_UTF8)
        : sqlite3_bind_null(statement, index);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(database));
}

void bind_optional_int64(sqlite3* database, sqlite3_stmt* statement,
                         const int index,
                         const std::optional<std::int64_t> value)
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

LocalActivityPulseCursor read_cursor(sqlite3_stmt* statement)
{
    const auto revision = sqlite3_column_int64(statement, 1);
    const auto generation = sqlite3_column_int64(statement, 2);
    const auto state_value = sqlite3_column_int(statement, 3);
    if (revision < 0 || generation < 0 || state_value < 0 || state_value > 4)
        throw std::runtime_error("invalid local activity pulse cursor row");

    LocalActivityPulseCursor cursor;
    cursor.scope = text(statement, 0);
    cursor.revision = static_cast<std::uint64_t>(revision);
    cursor.generation = static_cast<std::uint64_t>(generation);
    cursor.state = static_cast<LocalActivityPulseState>(state_value);
    cursor.anchor_checkpoint_task_id = text(statement, 4);
    cursor.anchor_checkpoint_result_sha256 = text(statement, 5);
    cursor.anchor_at_ms = sqlite3_column_int64(statement, 6);
    cursor.due_at_ms = sqlite3_column_int64(statement, 7);
    if (sqlite3_column_type(statement, 8) != SQLITE_NULL)
        cursor.captured_at_ms = sqlite3_column_int64(statement, 8);
    cursor.task_id = text(statement, 9);
    cursor.result_sha256 = optional_text(statement, 10);
    cursor.predecessor_observation_task_id = optional_text(statement, 11);
    cursor.predecessor_observation_result_sha256 = optional_text(statement, 12);
    cursor.blocked_reason = text(statement, 13);
    if (!valid_local_activity_pulse_cursor(cursor))
        throw std::runtime_error("non-canonical local activity pulse cursor row");
    return cursor;
}

constexpr const char* cursor_columns =
    "scope,revision,generation,state,anchor_checkpoint_task_id,"
    "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
    "task_id,result_sha256,predecessor_observation_task_id,"
    "predecessor_observation_result_sha256,blocked_reason";

std::int64_t user_table_count(sqlite3* database)
{
    Statement statement(database,
        "SELECT COUNT(*) FROM sqlite_master "
        "WHERE type='table' AND name NOT LIKE 'sqlite_%'");
    if (sqlite3_step(statement.get()) != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(database));
    return sqlite3_column_int64(statement.get(), 0);
}

void bind_cursor_values(sqlite3* database, sqlite3_stmt* statement,
                        const LocalActivityPulseCursor& cursor,
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
    bind_text(database, statement, index++, cursor.anchor_checkpoint_task_id);
    bind_text(database, statement, index++, cursor.anchor_checkpoint_result_sha256);
    if (sqlite3_bind_int64(statement, index++, cursor.anchor_at_ms) != SQLITE_OK
        || sqlite3_bind_int64(statement, index++, cursor.due_at_ms) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
    bind_optional_int64(database, statement, index++, cursor.captured_at_ms);
    bind_text(database, statement, index++, cursor.task_id);
    bind_optional_text(database, statement, index++, cursor.result_sha256);
    bind_optional_text(database, statement, index++, cursor.predecessor_observation_task_id);
    bind_optional_text(database, statement, index++,
                       cursor.predecessor_observation_result_sha256);
    bind_text(database, statement, index++, cursor.blocked_reason);
}

void verify_owner_mode(const std::string& path)
{
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0)
        throw std::runtime_error("cannot stat local activity pulse sidecar");
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "local activity pulse sidecar must be a current-user regular file mode 0600");
    }
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
        throw std::runtime_error("cannot securely create local activity pulse sidecar");
    verify_owner_mode(path);
}

} // namespace

bool valid_local_activity_pulse_cursor(
    const LocalActivityPulseCursor& cursor) noexcept
{
    if (cursor.scope != local_activity_pulse_scope
        || cursor.revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || cursor.generation > 3
        || !prefixed_sha256(cursor.anchor_checkpoint_task_id, checkpoint_prefix)
        || !lowercase_sha256(cursor.anchor_checkpoint_result_sha256)
        || cursor.anchor_at_ms < 0
        || cursor.due_at_ms < cursor.anchor_at_ms
        || (cursor.captured_at_ms && *cursor.captured_at_ms < cursor.anchor_at_ms)
        || cursor.task_id.size() > max_id_bytes
        || (!cursor.task_id.empty() && !safe_text(cursor.task_id))
        || !optional_safe_id(cursor.predecessor_observation_task_id)
        || !optional_sha(cursor.result_sha256)
        || !optional_sha(cursor.predecessor_observation_result_sha256)
        || cursor.blocked_reason.size() > max_blocked_reason_bytes
        || (!cursor.blocked_reason.empty() && !safe_text(cursor.blocked_reason))) {
        return false;
    }

    switch (cursor.state) {
    case LocalActivityPulseState::idle:
        return cursor.generation == 0
            && !cursor.captured_at_ms
            && cursor.task_id.empty()
            && !cursor.result_sha256
            && !cursor.predecessor_observation_task_id
            && !cursor.predecessor_observation_result_sha256
            && cursor.blocked_reason.empty();
    case LocalActivityPulseState::preparing:
        return active_generation_shape(cursor, false)
            && cursor.blocked_reason.empty();
    case LocalActivityPulseState::settled:
        return cursor.generation < 3
            && active_generation_shape(cursor, true)
            && cursor.blocked_reason.empty();
    case LocalActivityPulseState::quiescent:
        return cursor.generation == 3
            && active_generation_shape(cursor, true)
            && cursor.blocked_reason.empty();
    case LocalActivityPulseState::blocked:
        if (cursor.blocked_reason.empty()) return false;
        if (cursor.generation == 0) {
            return !cursor.captured_at_ms
                && cursor.task_id.empty()
                && !cursor.result_sha256
                && !cursor.predecessor_observation_task_id
                && !cursor.predecessor_observation_result_sha256;
        }
        return cursor.captured_at_ms
            && prefixed_sha256(cursor.task_id, observation_prefix)
            && predecessor_shape(cursor)
            && optional_sha(cursor.result_sha256);
    }
    return false;
}

bool valid_local_activity_pulse_transition(
    const LocalActivityPulseCursor& expected,
    const LocalActivityPulseCursor& replacement) noexcept
{
    if (!valid_local_activity_pulse_cursor(expected)
        || !valid_local_activity_pulse_cursor(replacement)
        || expected.scope != replacement.scope
        || expected.anchor_checkpoint_task_id != replacement.anchor_checkpoint_task_id
        || expected.anchor_checkpoint_result_sha256
            != replacement.anchor_checkpoint_result_sha256
        || expected.anchor_at_ms != replacement.anchor_at_ms
        || expected.revision == static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || replacement.revision != expected.revision + 1
        || replacement.generation < expected.generation
        || replacement.generation > expected.generation + 1
        || expected.state == LocalActivityPulseState::quiescent) {
        return false;
    }

    switch (expected.state) {
    case LocalActivityPulseState::idle:
        return (replacement.state == LocalActivityPulseState::preparing
                && replacement.generation == 1)
            || (replacement.state == LocalActivityPulseState::blocked
                && replacement.generation == 0);
    case LocalActivityPulseState::preparing:
        return replacement.generation == expected.generation
            && (replacement.state == LocalActivityPulseState::settled
                || replacement.state == LocalActivityPulseState::quiescent
                || replacement.state == LocalActivityPulseState::blocked);
    case LocalActivityPulseState::settled:
        return (replacement.state == LocalActivityPulseState::preparing
                && replacement.generation == expected.generation + 1)
            || (replacement.state == LocalActivityPulseState::blocked
                && replacement.generation == expected.generation);
    case LocalActivityPulseState::blocked:
        return replacement.state == LocalActivityPulseState::blocked
            && replacement.generation == expected.generation;
    case LocalActivityPulseState::quiescent:
        return false;
    }
    return false;
}

LocalActivityPulseStore::LocalActivityPulseStore(const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_ ? sqlite3_errmsg(database_)
                                               : "cannot open local activity sidecar SQLite";
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
        if (version > local_activity_pulse_sidecar_schema)
            throw std::runtime_error("unsupported local activity pulse sidecar schema");

        if (version == 0) {
            if (user_table_count(database_) != 0)
                throw std::runtime_error("unversioned local activity sidecar is not empty");
            execute(database_,
                "CREATE TABLE local_activity_pulse_cursor ("
                " scope TEXT PRIMARY KEY NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " generation INTEGER NOT NULL CHECK(generation BETWEEN 0 AND 3),"
                " state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
                " anchor_checkpoint_task_id TEXT NOT NULL,"
                " anchor_checkpoint_result_sha256 TEXT NOT NULL,"
                " anchor_at_ms INTEGER NOT NULL CHECK(anchor_at_ms >= 0),"
                " due_at_ms INTEGER NOT NULL CHECK(due_at_ms >= anchor_at_ms),"
                " captured_at_ms INTEGER CHECK(captured_at_ms IS NULL OR captured_at_ms >= anchor_at_ms),"
                " task_id TEXT NOT NULL,"
                " result_sha256 TEXT,"
                " predecessor_observation_task_id TEXT,"
                " predecessor_observation_result_sha256 TEXT,"
                " blocked_reason TEXT NOT NULL"
                ");");
            execute(database_, "PRAGMA user_version=1;");
        } else {
            Statement schema_probe(database_,
                "SELECT scope,revision,generation,state,anchor_checkpoint_task_id,"
                "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
                "task_id,result_sha256,predecessor_observation_task_id,"
                "predecessor_observation_result_sha256,blocked_reason "
                "FROM local_activity_pulse_cursor LIMIT 0");
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

LocalActivityPulseStore::~LocalActivityPulseStore()
{
    sqlite3_close(database_);
}

std::optional<LocalActivityPulseCursor>
LocalActivityPulseStore::find(const std::string& scope) const
{
    const std::string sql = std::string{"SELECT "} + cursor_columns
        + " FROM local_activity_pulse_cursor WHERE scope=?1 LIMIT 1";
    Statement statement(database_, sql.c_str());
    bind_text(database_, statement.get(), 1, scope);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
    return read_cursor(statement.get());
}

LocalActivityPulseStoreWrite LocalActivityPulseStore::seed(
    const LocalActivityPulseCursor& cursor)
{
    if (!valid_local_activity_pulse_cursor(cursor)
        || cursor.revision != 0
        || cursor.generation != 0
        || cursor.state != LocalActivityPulseState::idle) {
        return {LocalActivityPulseStoreResult::invalid, {},
                "local activity seed cursor is non-canonical"};
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = find(cursor.scope);
        if (existing) {
            execute(database_, "ROLLBACK;");
            return same_cursor(*existing, cursor)
                ? LocalActivityPulseStoreWrite{
                    LocalActivityPulseStoreResult::duplicate, existing, {}}
                : LocalActivityPulseStoreWrite{
                    LocalActivityPulseStoreResult::conflict, existing,
                    "local activity sidecar is already seeded differently"};
        }

        Statement statement(database_,
            "INSERT INTO local_activity_pulse_cursor ("
            "scope,revision,generation,state,anchor_checkpoint_task_id,"
            "anchor_checkpoint_result_sha256,anchor_at_ms,due_at_ms,captured_at_ms,"
            "task_id,result_sha256,predecessor_observation_task_id,"
            "predecessor_observation_result_sha256,blocked_reason) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)");
        bind_text(database_, statement.get(), 1, cursor.scope);
        bind_cursor_values(database_, statement.get(), cursor, 2);
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));
        execute(database_, "COMMIT;");
        const auto stored = find(cursor.scope);
        if (!stored || !same_cursor(*stored, cursor))
            return {LocalActivityPulseStoreResult::conflict, stored,
                    "local activity seed did not persist exact cursor"};
        return {LocalActivityPulseStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalActivityPulseStoreResult::unavailable, {}, error.what()};
    }
}

LocalActivityPulseStoreWrite LocalActivityPulseStore::replace(
    const LocalActivityPulseCursor& expected,
    const LocalActivityPulseCursor& replacement)
{
    if (!valid_local_activity_pulse_transition(expected, replacement)) {
        return {LocalActivityPulseStoreResult::invalid, {},
                "local activity cursor transition is non-canonical"};
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        Statement statement(database_,
            "UPDATE local_activity_pulse_cursor SET "
            "revision=?1,generation=?2,state=?3,anchor_checkpoint_task_id=?4,"
            "anchor_checkpoint_result_sha256=?5,anchor_at_ms=?6,due_at_ms=?7,"
            "captured_at_ms=?8,task_id=?9,result_sha256=?10,"
            "predecessor_observation_task_id=?11,"
            "predecessor_observation_result_sha256=?12,blocked_reason=?13 "
            "WHERE scope=?14 AND revision=?15");
        bind_cursor_values(database_, statement.get(), replacement, 1);
        bind_text(database_, statement.get(), 14, expected.scope);
        if (sqlite3_bind_int64(statement.get(), 15,
                static_cast<sqlite3_int64>(expected.revision)) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database_));
        if (sqlite3_step(statement.get()) != SQLITE_DONE)
            throw std::runtime_error(sqlite3_errmsg(database_));

        if (sqlite3_changes(database_) != 1) {
            const auto current = find(expected.scope);
            execute(database_, "ROLLBACK;");
            if (current && same_cursor(*current, replacement))
                return {LocalActivityPulseStoreResult::duplicate, current, {}};
            return {LocalActivityPulseStoreResult::conflict, current,
                    "local activity cursor revision changed concurrently"};
        }

        execute(database_, "COMMIT;");
        const auto stored = find(expected.scope);
        if (!stored || !same_cursor(*stored, replacement))
            return {LocalActivityPulseStoreResult::conflict, stored,
                    "local activity cursor replacement did not persist exactly"};
        return {LocalActivityPulseStoreResult::accepted, stored, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalActivityPulseStoreResult::unavailable, {}, error.what()};
    }
}

} // namespace gaudere_agent
