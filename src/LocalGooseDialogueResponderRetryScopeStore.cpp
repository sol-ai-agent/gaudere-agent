#include "LocalGooseDialogueResponderRetryScopeStore.hpp"

#include "Sha256.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

using Scope = LocalGooseDialogueResponderRetryScope;
using Result = LocalGooseDialogueResponderRetryScopeResult;
constexpr const char* table_name = "local_goose_dialogue_responder_retry_scope";
constexpr const char* schema_sql =
    "CREATE TABLE local_goose_dialogue_responder_retry_scope("
    "scope_id TEXT PRIMARY KEY NOT NULL,"
    "request_id TEXT NOT NULL UNIQUE,"
    "lineage_id TEXT NOT NULL UNIQUE,"
    "consumer_id TEXT NOT NULL,"
    "event_id TEXT NOT NULL,"
    "result_sha256 TEXT NOT NULL,"
    "task_id TEXT NOT NULL,"
    "thread_alias TEXT NOT NULL,"
    "observed_speaker_kind TEXT NOT NULL,"
    "observed_speaker_id TEXT NOT NULL,"
    "retry_generation INTEGER NOT NULL CHECK(retry_generation=1),"
    "completion_sequence INTEGER NOT NULL CHECK(completion_sequence=4),"
    "expected_head_revision INTEGER NOT NULL CHECK(expected_head_revision=3),"
    "prepared_at_ms INTEGER NOT NULL CHECK(prepared_at_ms>=0),"
    "UNIQUE(consumer_id,event_id,retry_generation),"\n    "UNIQUE(consumer_id,completion_sequence,retry_generation))";

constexpr const char* select_columns =
    "scope_id,request_id,lineage_id,consumer_id,event_id,result_sha256,"
    "task_id,thread_alias,observed_speaker_kind,observed_speaker_id,"
    "retry_generation,completion_sequence,expected_head_revision,prepared_at_ms";

class Statement {
public:
    Statement(sqlite3* database, const std::string& sql) : database_(database)
    {
        if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement_, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3* database_;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* db, const char* sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

std::string column_text(sqlite3_stmt* s, const int column)
{
    const auto* p = sqlite3_column_text(s, column);
    const int n = sqlite3_column_bytes(s, column);
    if (!p || n < 0) return {};
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<std::size_t>(n));
}

void bind_text(sqlite3_stmt* s, const int column, const std::string& value)
{
    if (sqlite3_bind_text(s, column, value.c_str(), -1, SQLITE_TRANSIENT)
        != SQLITE_OK) {
        throw std::runtime_error("retry scope bind failure");
    }
}

void bind_i64(sqlite3_stmt* s, const int column, const std::int64_t value)
{
    if (sqlite3_bind_int64(s, column, value) != SQLITE_OK)
        throw std::runtime_error("retry scope integer bind failure");
}

bool safe_token(const std::string& s) noexcept
{
    if (s.empty() || s.size() > 128) return false;
    for (const unsigned char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.'
              || c == ':' || c == '-')) return false;
    }
    return true;
}

bool sha256_lower(const std::string& s) noexcept
{
    if (s.size() != 64) return false;
    for (const unsigned char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool prefixed_sha(const std::string& s, const char* prefix) noexcept
{
    const std::string p(prefix);
    return s.size() == p.size() + 64
        && s.compare(0, p.size(), p) == 0
        && sha256_lower(s.substr(p.size()));
}

void append(std::string& out, const std::string& v)
{
    out += std::to_string(v.size()) + ":" + v + "|";
}

void append(std::string& out, const std::uint64_t v)
{
    out += std::to_string(v) + "|";
}

std::string scope_identity(const Scope& s)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-retry-scope.v1|";
    append(canonical, s.request_id);
    append(canonical, s.lineage_id);
    append(canonical, s.consumer_id);
    append(canonical, s.event_id);
    append(canonical, s.result_sha256);
    append(canonical, s.task_id);
    append(canonical, s.thread_alias);
    append(canonical, s.observed_speaker_kind);
    append(canonical, s.observed_speaker_id);
    append(canonical, static_cast<std::uint64_t>(s.retry_generation));
    append(canonical, s.completion_sequence);
    append(canonical, s.expected_head_revision);
    append(canonical, static_cast<std::uint64_t>(s.prepared_at_ms));
    return std::string(local_goose_dialogue_responder_retry_scope_prefix)
        + sha256_hex(canonical);
}

bool equal_scope(const Scope& a, const Scope& b)
{
    return a.scope_id == b.scope_id
        && a.request_id == b.request_id
        && a.lineage_id == b.lineage_id
        && a.consumer_id == b.consumer_id
        && a.event_id == b.event_id
        && a.result_sha256 == b.result_sha256
        && a.task_id == b.task_id
        && a.thread_alias == b.thread_alias
        && a.observed_speaker_kind == b.observed_speaker_kind
        && a.observed_speaker_id == b.observed_speaker_id
        && a.retry_generation == b.retry_generation
        && a.completion_sequence == b.completion_sequence
        && a.expected_head_revision == b.expected_head_revision
        && a.prepared_at_ms == b.prepared_at_ms;
}

void check_owner_file(const std::string& path)
{
    struct stat s {};
    if (lstat(path.c_str(), &s) != 0 || !S_ISREG(s.st_mode)
        || s.st_uid != geteuid() || (s.st_mode & 0777) != 0600)
        throw std::runtime_error("retry scope requires owner regular 0600 file");
}

void reject_companions(const std::string& path)
{
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        struct stat s {};
        const std::string candidate = path + suffix;
        if (lstat(candidate.c_str(), &s) == 0 || errno != ENOENT)
            throw std::runtime_error("retry scope SQLite companion present");
    }
}

void create_secure(const std::string& path)
{
    const int fd = open(path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) throw std::runtime_error("retry scope file already exists or unsafe");
    close(fd);
    check_owner_file(path);
}

void verify_schema(sqlite3* db)
{
    {
        Statement q(db, "PRAGMA user_version");
        if (sqlite3_step(q.get()) != SQLITE_ROW
            || sqlite3_column_int(q.get(), 0)
                != local_goose_dialogue_responder_retry_scope_schema
            || sqlite3_step(q.get()) != SQLITE_DONE)
            throw std::runtime_error("unexpected retry scope schema version");
    }
    {
        Statement q(db, "PRAGMA journal_mode");
        if (sqlite3_step(q.get()) != SQLITE_ROW
            || column_text(q.get(), 0) != "delete"
            || sqlite3_step(q.get()) != SQLITE_DONE)
            throw std::runtime_error("retry scope must use DELETE journal");
    }
    {
        Statement q(db, "SELECT type,name,sql FROM sqlite_master "
                        "WHERE name NOT LIKE 'sqlite_%'");
        if (sqlite3_step(q.get()) != SQLITE_ROW
            || column_text(q.get(), 0) != "table"
            || column_text(q.get(), 1) != table_name
            || column_text(q.get(), 2) != schema_sql
            || sqlite3_step(q.get()) != SQLITE_DONE)
            throw std::runtime_error("unexpected retry scope objects/DDL");
    }
}

Scope decode(sqlite3_stmt* q)
{
    Scope s;
    s.scope_id = column_text(q, 0);
    s.request_id = column_text(q, 1);
    s.lineage_id = column_text(q, 2);
    s.consumer_id = column_text(q, 3);
    s.event_id = column_text(q, 4);
    s.result_sha256 = column_text(q, 5);
    s.task_id = column_text(q, 6);
    s.thread_alias = column_text(q, 7);
    s.observed_speaker_kind = column_text(q, 8);
    s.observed_speaker_id = column_text(q, 9);
    const auto gen = sqlite3_column_int64(q, 10);
    const auto seq = sqlite3_column_int64(q, 11);
    const auto head = sqlite3_column_int64(q, 12);
    s.prepared_at_ms = sqlite3_column_int64(q, 13);
    if (gen != 1 || seq != 4 || head != 3)
        throw std::runtime_error("invalid retry scope indexed counters");
    s.retry_generation = static_cast<std::uint32_t>(gen);
    s.completion_sequence = static_cast<std::uint64_t>(seq);
    s.expected_head_revision = static_cast<std::uint64_t>(head);
    if (!valid_local_goose_dialogue_responder_retry_scope(s))
        throw std::runtime_error("noncanonical retry scope row");
    return s;
}

std::optional<Scope> find(sqlite3* db,
                          const char* column,
                          const std::string& value)
{
    const std::string sql = std::string("SELECT ") + select_columns
        + " FROM " + table_name + " WHERE " + column + "=?1";
    Statement q(db, sql);
    bind_text(q.get(), 1, value);
    const int step = sqlite3_step(q.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    auto s = decode(q.get());
    if (sqlite3_step(q.get()) != SQLITE_DONE)
        throw std::runtime_error("duplicate retry scope index");
    return s;
}

} // namespace

std::optional<Scope> make_local_goose_dialogue_responder_retry_scope(
    const std::string& request_id,
    const LocalGooseDialogueResponderRetryLineage& lineage,
    const std::int64_t prepared_at_ms)
{
    if (!valid_local_goose_dialogue_responder_retry_lineage(lineage))
        return std::nullopt;
    Scope s;
    s.request_id = request_id;
    s.lineage_id = lineage.lineage_id;
    s.consumer_id = lineage.consumer_id;
    s.event_id = lineage.event_id;
    s.result_sha256 = lineage.result_sha256;
    s.task_id = lineage.task_id;
    s.thread_alias = lineage.thread_alias;
    s.observed_speaker_kind = lineage.observed_speaker_kind;
    s.observed_speaker_id = lineage.observed_speaker_id;
    s.retry_generation = lineage.retry_generation;
    s.completion_sequence = lineage.completion_sequence;
    s.expected_head_revision = lineage.thread_revision;
    s.prepared_at_ms = prepared_at_ms;
    if (prepared_at_ms < 0) return std::nullopt;
    s.scope_id = scope_identity(s);
    if (!valid_local_goose_dialogue_responder_retry_scope(s))
        return std::nullopt;
    return s;
}

bool valid_local_goose_dialogue_responder_retry_scope(const Scope& s) noexcept
{
    if (!prefixed_sha(s.scope_id,
            local_goose_dialogue_responder_retry_scope_prefix)
        || !safe_token(s.request_id)
        || !prefixed_sha(s.lineage_id,
            local_goose_dialogue_responder_retry_lineage_prefix)
        || s.consumer_id != "system-responder-v1"
        || !prefixed_sha(s.event_id,
            "cognition.local-goose-dialogue.completion.v1:")
        || !sha256_lower(s.result_sha256)
        || !prefixed_sha(s.task_id,
            "cognition.local-goose-dialogue.v3:")
        || s.thread_alias != "main"
        || s.observed_speaker_kind != "human"
        || s.observed_speaker_id != "bertrand"
        || s.retry_generation != 1
        || s.completion_sequence != 4
        || s.expected_head_revision != 3
        || s.prepared_at_ms < 0)
        return false;
    try {
        return s.scope_id == scope_identity(s);
    } catch (...) {
        return false;
    }
}

LocalGooseDialogueResponderRetryScopeStore::
LocalGooseDialogueResponderRetryScopeStore(
    const std::string& path,
    const LocalGooseDialogueResponderRetryScopeOpenMode mode)
{
    using Mode = LocalGooseDialogueResponderRetryScopeOpenMode;
    if (mode != Mode::create_new_isolated && mode != Mode::existing_read_only
        && mode != Mode::existing_writer)
        throw std::invalid_argument("unknown retry scope open mode");
    read_only_ = mode == Mode::existing_read_only;
    const bool fresh = mode == Mode::create_new_isolated;
    if (fresh) create_secure(path);
    else check_owner_file(path);
    reject_companions(path);
    const int flags = SQLITE_OPEN_FULLMUTEX
        | (read_only_ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE);
    try {
        if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot open retry scope SQLite");
        if (sqlite3_db_readonly(database_, "main") != (read_only_ ? 1 : 0))
            throw std::runtime_error("retry scope SQLite access mode differs");
        if (fresh) {
            execute(database_, "PRAGMA journal_mode=DELETE");
            execute(database_, schema_sql);
            execute(database_, "PRAGMA user_version=1");
        }
        verify_schema(database_);
        schema_version_ = local_goose_dialogue_responder_retry_scope_schema;
    } catch (...) {
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderRetryScopeStore::
~LocalGooseDialogueResponderRetryScopeStore()
{
    if (database_) sqlite3_close(database_);
}

std::optional<Scope>
LocalGooseDialogueResponderRetryScopeStore::find_by_request(
    const std::string& request_id) const
{
    return find(database_, "request_id", request_id);
}

std::optional<Scope>
LocalGooseDialogueResponderRetryScopeStore::find_by_lineage(
    const std::string& lineage_id) const
{
    return find(database_, "lineage_id", lineage_id);
}

LocalGooseDialogueResponderRetryScopeWrite
LocalGooseDialogueResponderRetryScopeStore::stage_scope(const Scope& scope)
{
    if (read_only_) return {Result::unavailable, std::nullopt};
    if (!valid_local_goose_dialogue_responder_retry_scope(scope))
        return {Result::invalid, std::nullopt};
    bool in_transaction = false;
    try {
        execute(database_, "BEGIN IMMEDIATE");
        in_transaction = true;
        const auto for_lineage = find_by_lineage(scope.lineage_id);
        const auto for_request = find_by_request(scope.request_id);
        if (for_lineage || for_request) {
            const bool exact = for_lineage && for_request
                && equal_scope(*for_lineage, scope)
                && equal_scope(*for_request, scope);
            execute(database_, "COMMIT");
            in_transaction = false;
            return {exact ? Result::duplicate : Result::conflict,
                    exact ? for_lineage : std::nullopt};
        }
        const std::string sql = std::string("INSERT INTO ") + table_name
            + "(" + select_columns + ") VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)";
        Statement q(database_, sql);
        bind_text(q.get(), 1, scope.scope_id);
        bind_text(q.get(), 2, scope.request_id);
        bind_text(q.get(), 3, scope.lineage_id);
        bind_text(q.get(), 4, scope.consumer_id);
        bind_text(q.get(), 5, scope.event_id);
        bind_text(q.get(), 6, scope.result_sha256);
        bind_text(q.get(), 7, scope.task_id);
        bind_text(q.get(), 8, scope.thread_alias);
        bind_text(q.get(), 9, scope.observed_speaker_kind);
        bind_text(q.get(), 10, scope.observed_speaker_id);
        bind_i64(q.get(), 11, scope.retry_generation);
        bind_i64(q.get(), 12, static_cast<std::int64_t>(scope.completion_sequence));
        bind_i64(q.get(), 13, static_cast<std::int64_t>(scope.expected_head_revision));
        bind_i64(q.get(), 14, scope.prepared_at_ms);
        const int step = sqlite3_step(q.get());
        if (step != SQLITE_DONE) {
            if (step == SQLITE_CONSTRAINT || (step & 0xff) == SQLITE_CONSTRAINT) {
                execute(database_, "ROLLBACK");
                in_transaction = false;
                return {Result::conflict, std::nullopt};
            }
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT");
        in_transaction = false;
        return {Result::accepted, scope};
    } catch (...) {
        if (in_transaction) {
            try { execute(database_, "ROLLBACK"); } catch (...) {}
        }
        return {Result::unavailable, std::nullopt};
    }
}

} // namespace gaudere_agent
