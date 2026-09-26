#include "LocalGooseDialogueThreadStore.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

constexpr const char* v2_prefix = "cognition.local-goose-dialogue.v2:";
constexpr const char* v3_prefix = "cognition.local-goose-dialogue.v3:";
constexpr std::size_t max_alias_bytes = 128;

class Statement {
public:
    Statement(sqlite3* database, const char* sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
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

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
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

bool dialogue_task_id(const std::string& value) noexcept
{
    return prefixed_sha256(value, v2_prefix)
        || prefixed_sha256(value, v3_prefix);
}

bool safe_alias(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_alias_bytes) return false;
    for (const unsigned char character : value) {
        const bool allowed =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_'
            || character == ':' || character == '-';
        if (!allowed) return false;
    }
    return true;
}

bool same_head(const LocalGooseDialogueThreadHead& left,
               const LocalGooseDialogueThreadHead& right) noexcept
{
    return left.alias == right.alias
        && left.revision == right.revision
        && left.root_task_id == right.root_task_id
        && left.head_task_id == right.head_task_id;
}

void bind_text(sqlite3* database,
               sqlite3_stmt* statement,
               const int index,
               const std::string& value)
{
    if (sqlite3_bind_text64(statement, index, value.data(), value.size(),
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

LocalGooseDialogueThreadHead read_head(sqlite3_stmt* statement)
{
    const auto revision = sqlite3_column_int64(statement, 1);
    if (revision < 0) {
        throw std::runtime_error("invalid Local Goose dialogue thread revision");
    }

    LocalGooseDialogueThreadHead head;
    head.alias = text(statement, 0);
    head.revision = static_cast<std::uint64_t>(revision);
    head.root_task_id = text(statement, 2);
    head.head_task_id = text(statement, 3);
    if (!valid_local_goose_dialogue_thread_head(head)) {
        throw std::runtime_error(
            "non-canonical Local Goose dialogue thread head row");
    }
    return head;
}

std::optional<LocalGooseDialogueThreadHead> find_head(
    sqlite3* database,
    const std::string& alias)
{
    Statement statement(
        database,
        "SELECT alias,revision,root_task_id,head_task_id "
        "FROM local_goose_dialogue_thread_head WHERE alias=?1");
    bind_text(database, statement.get(), 1, alias);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto head = read_head(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "duplicate Local Goose dialogue thread head row");
    }
    return head;
}

std::vector<LocalGooseDialogueThreadHead> read_history_from(
    sqlite3* database,
    const std::string& alias,
    const std::uint64_t first_revision,
    const std::size_t limit)
{
    std::vector<LocalGooseDialogueThreadHead> out;
    Statement statement(
        database,
        "SELECT alias,revision,root_task_id,head_task_id "
        "FROM local_goose_dialogue_thread_history "
        "WHERE alias=?1 AND revision>=?2 "
        "ORDER BY revision ASC LIMIT ?3");
    bind_text(database, statement.get(), 1, alias);
    if (sqlite3_bind_int64(
            statement.get(), 2,
            static_cast<sqlite3_int64>(first_revision)) != SQLITE_OK
        || sqlite3_bind_int64(
            statement.get(), 3,
            static_cast<sqlite3_int64>(limit)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
        out.push_back(read_head(statement.get()));
    }
    return out;
}

void insert_history(
    sqlite3* database,
    const LocalGooseDialogueThreadHead& head)
{
    Statement statement(
        database,
        "INSERT INTO local_goose_dialogue_thread_history"
        "(alias,revision,root_task_id,head_task_id)"
        " VALUES(?1,?2,?3,?4)");
    bind_text(database, statement.get(), 1, head.alias);
    if (sqlite3_bind_int64(
            statement.get(), 2,
            static_cast<sqlite3_int64>(head.revision)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    bind_text(database, statement.get(), 3, head.root_task_id);
    bind_text(database, statement.get(), 4, head.head_task_id);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

std::int64_t user_table_count(sqlite3* database)
{
    Statement statement(
        database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%'");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    return sqlite3_column_int64(statement.get(), 0);
}

void verify_owner_mode(const std::string& path)
{
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error(
            "cannot stat Local Goose dialogue thread sidecar");
    }
    if (!S_ISREG(status.st_mode)
        || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "Local Goose dialogue thread sidecar must be a current-user "
            "regular file mode 0600");
    }
}

void ensure_secure_file(const std::string& path)
{
    const int descriptor = open(
        path.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);
    if (descriptor >= 0) {
        close(descriptor);
        verify_owner_mode(path);
        return;
    }
    if (errno != EEXIST) {
        throw std::runtime_error(
            "cannot securely create Local Goose dialogue thread sidecar");
    }
    verify_owner_mode(path);
}

} // namespace

bool valid_local_goose_dialogue_thread_head(
    const LocalGooseDialogueThreadHead& head) noexcept
{
    return safe_alias(head.alias)
        && head.revision <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        && dialogue_task_id(head.root_task_id)
        && dialogue_task_id(head.head_task_id);
}

bool valid_local_goose_dialogue_thread_head_transition(
    const LocalGooseDialogueThreadHead& expected,
    const LocalGooseDialogueThreadHead& replacement) noexcept
{
    return valid_local_goose_dialogue_thread_head(expected)
        && valid_local_goose_dialogue_thread_head(replacement)
        && expected.alias == replacement.alias
        && expected.root_task_id == replacement.root_task_id
        && expected.revision < static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        && replacement.revision == expected.revision + 1
        && replacement.head_task_id != expected.head_task_id;
}

LocalGooseDialogueThreadStore::LocalGooseDialogueThreadStore(
    const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_)
            : "cannot open Local Goose dialogue thread sidecar SQLite";
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
        if (sqlite3_step(version_statement.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        const int version =
            sqlite3_column_int(version_statement.get(), 0);
        if (version > local_goose_dialogue_thread_sidecar_schema) {
            throw std::runtime_error(
                "unsupported Local Goose dialogue thread sidecar schema");
        }

        if (version == 0) {
            if (user_table_count(database_) != 0) {
                throw std::runtime_error(
                    "unversioned Local Goose dialogue thread sidecar is not empty");
            }
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_thread_head ("
                " alias TEXT PRIMARY KEY NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " root_task_id TEXT NOT NULL,"
                " head_task_id TEXT NOT NULL"
                ");");
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_thread_history ("
                " alias TEXT NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " root_task_id TEXT NOT NULL,"
                " head_task_id TEXT NOT NULL,"
                " PRIMARY KEY(alias,revision)"
                ");");
            execute(database_, "PRAGMA user_version=2;");
        } else if (version == 1) {
            if (user_table_count(database_) != 1) {
                throw std::runtime_error(
                    "Local Goose dialogue thread v1 table set differs");
            }
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_thread_history ("
                " alias TEXT NOT NULL,"
                " revision INTEGER NOT NULL CHECK(revision >= 0),"
                " root_task_id TEXT NOT NULL,"
                " head_task_id TEXT NOT NULL,"
                " PRIMARY KEY(alias,revision)"
                ");");
            execute(
                database_,
                "INSERT INTO local_goose_dialogue_thread_history"
                "(alias,revision,root_task_id,head_task_id) "
                "SELECT alias,revision,root_task_id,head_task_id "
                "FROM local_goose_dialogue_thread_head;");
            execute(database_, "PRAGMA user_version=2;");
        } else if (user_table_count(database_) != 2) {
            throw std::runtime_error(
                "Local Goose dialogue thread sidecar table set differs");
        }

        execute(database_, "COMMIT;");
    } catch (...) {
        try {
            execute(database_, "ROLLBACK;");
        } catch (...) {
        }
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueThreadStore::~LocalGooseDialogueThreadStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseDialogueThreadHead>
LocalGooseDialogueThreadStore::find(const std::string& alias) const
{
    if (!safe_alias(alias)) return std::nullopt;
    return find_head(database_, alias);
}

std::vector<LocalGooseDialogueThreadHead>
LocalGooseDialogueThreadStore::history_from(
    const std::string& alias,
    const std::uint64_t first_revision,
    const std::size_t limit) const
{
    if (!safe_alias(alias)
        || first_revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || limit == 0 || limit > 1024) {
        return {};
    }
    return read_history_from(database_, alias, first_revision, limit);
}

LocalGooseDialogueThreadStoreWrite
LocalGooseDialogueThreadStore::seed(
    const LocalGooseDialogueThreadHead& head)
{
    LocalGooseDialogueThreadStoreWrite out;
    if (!valid_local_goose_dialogue_thread_head(head)
        || head.revision != 0) {
        out.detail = "invalid initial Local Goose dialogue thread head";
        return out;
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto current = find_head(database_, head.alias);
        if (current) {
            execute(database_, "COMMIT;");
            out.head = current;
            if (same_head(*current, head)) {
                out.result = LocalGooseDialogueThreadStoreResult::duplicate;
                out.detail = "dialogue thread head already seeded";
            } else {
                out.result = LocalGooseDialogueThreadStoreResult::conflict;
                out.detail = "dialogue thread alias is already bound";
            }
            return out;
        }

        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_thread_head"
            "(alias,revision,root_task_id,head_task_id)"
            " VALUES(?1,?2,?3,?4)");
        bind_text(database_, statement.get(), 1, head.alias);
        if (sqlite3_bind_int64(
                statement.get(), 2,
                static_cast<sqlite3_int64>(head.revision)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(database_, statement.get(), 3, head.root_task_id);
        bind_text(database_, statement.get(), 4, head.head_task_id);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        insert_history(database_, head);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueThreadStoreResult::accepted;
        out.head = head;
        out.detail = "dialogue thread head seeded";
        return out;
    } catch (const std::exception& error) {
        try {
            execute(database_, "ROLLBACK;");
        } catch (...) {
        }
        out.result = LocalGooseDialogueThreadStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueThreadStoreWrite
LocalGooseDialogueThreadStore::replace(
    const LocalGooseDialogueThreadHead& expected,
    const LocalGooseDialogueThreadHead& replacement)
{
    LocalGooseDialogueThreadStoreWrite out;
    if (!valid_local_goose_dialogue_thread_head_transition(
            expected, replacement)) {
        out.detail = "invalid Local Goose dialogue thread head transition";
        return out;
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto current = find_head(database_, expected.alias);
        if (!current) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueThreadStoreResult::conflict;
            out.detail = "dialogue thread head is missing";
            return out;
        }
        if (same_head(*current, replacement)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueThreadStoreResult::duplicate;
            out.head = current;
            out.detail = "dialogue thread head transition already applied";
            return out;
        }
        if (!same_head(*current, expected)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueThreadStoreResult::conflict;
            out.head = current;
            out.detail = "dialogue thread head compare-and-swap conflict";
            return out;
        }

        Statement statement(
            database_,
            "UPDATE local_goose_dialogue_thread_head "
            "SET revision=?1,head_task_id=?2 "
            "WHERE alias=?3 AND revision=?4 "
            "AND root_task_id=?5 AND head_task_id=?6");
        if (sqlite3_bind_int64(
                statement.get(), 1,
                static_cast<sqlite3_int64>(replacement.revision)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(database_, statement.get(), 2, replacement.head_task_id);
        bind_text(database_, statement.get(), 3, expected.alias);
        if (sqlite3_bind_int64(
                statement.get(), 4,
                static_cast<sqlite3_int64>(expected.revision)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(database_, statement.get(), 5, expected.root_task_id);
        bind_text(database_, statement.get(), 6, expected.head_task_id);
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        if (sqlite3_changes(database_) != 1) {
            throw std::runtime_error(
                "dialogue thread head CAS changed unexpected row count");
        }
        insert_history(database_, replacement);

        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueThreadStoreResult::accepted;
        out.head = replacement;
        out.detail = "dialogue thread head advanced";
        return out;
    } catch (const std::exception& error) {
        try {
            execute(database_, "ROLLBACK;");
        } catch (...) {
        }
        out.result = LocalGooseDialogueThreadStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

std::string local_goose_dialogue_thread_head_report(
    const LocalGooseDialogueThreadHead& head)
{
    if (!valid_local_goose_dialogue_thread_head(head)) {
        throw std::invalid_argument(
            "cannot report invalid Local Goose dialogue thread head");
    }
    std::ostringstream output;
    output << "alias=\"" << head.alias << "\"\n"
           << "revision=" << head.revision << '\n'
           << "root_task_id=\"" << head.root_task_id << "\"\n"
           << "head_task_id=\"" << head.head_task_id << "\"\n";
    return output.str();
}

} // namespace gaudere_agent
