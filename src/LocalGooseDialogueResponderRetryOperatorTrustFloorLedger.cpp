#include "LocalGooseDialogueResponderRetryOperatorTrustFloorLedger.hpp"

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

using Mode = LocalGooseDialogueResponderRetryTrustFloorOpenMode;
using Code = LocalGooseDialogueResponderRetryTrustFloorCode;
using Record = LocalGooseDialogueResponderRetryTrustFloorRecord;

constexpr const char* table_name =
    "local_goose_dialogue_responder_retry_trust_floor";
constexpr const char* schema_sql =
    "CREATE TABLE local_goose_dialogue_responder_retry_trust_floor("
    "slot INTEGER PRIMARY KEY NOT NULL CHECK(slot=1),"
    "root_fingerprint TEXT NOT NULL,"
    "minimum_generation INTEGER NOT NULL CHECK(minimum_generation>=1),"
    "last_transition_sha256 TEXT NOT NULL)";

bool lower_sha256(const std::string& s) noexcept
{
    if (s.size() != 64) return false;
    for (const unsigned char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool safe_path(const std::string& path)
{
    if (path.size() < 2 || path.size() > 4096 || path.front() != '/'
        || path.find(char(0)) != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto size = (end == std::string::npos ? path.size() : end) - start;
        if (size == 0 || (size == 1 && path[start] == '.')
            || (size == 2 && path.compare(start, size, "..") == 0))
            return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

void require_safe_parent(const std::string& path)
{
    if (!safe_path(path))
        throw std::runtime_error("invalid trust floor path");
    const auto split = path.find_last_of('/');
    const std::string parent = split == 0 ? "/" : path.substr(0, split);
    struct stat s {};
    if (lstat(parent.c_str(), &s) != 0
        || !S_ISDIR(s.st_mode)
        || s.st_uid != geteuid()
        || (s.st_mode & 0777) != 0700)
        throw std::runtime_error("trust floor parent must be owner-only 0700");
}

struct stat require_safe_file(const std::string& path)
{
    struct stat s {};
    if (lstat(path.c_str(), &s) != 0
        || !S_ISREG(s.st_mode)
        || s.st_uid != geteuid()
        || (s.st_mode & 0777) != 0600
        || s.st_nlink != 1)
        throw std::runtime_error("trust floor file must be owner-only regular 0600");
    return s;
}

void reject_companions(const std::string& path)
{
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        const std::string candidate = path + suffix;
        struct stat s {};
        if (lstat(candidate.c_str(), &s) == 0 || errno != ENOENT)
            throw std::runtime_error("unsafe trust floor SQLite companion");
    }
}

void create_exclusive(const std::string& path)
{
    const int fd = open(path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("trust floor file already exists or unsafe");
    close(fd);
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return stmt_; }
private:
    sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* sql)
{
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void bind_text(sqlite3_stmt* stmt, const int i, const std::string& value)
{
    if (sqlite3_bind_text(stmt, i, value.c_str(), -1, SQLITE_TRANSIENT)
        != SQLITE_OK)
        throw std::runtime_error("trust floor bind text failed");
}

void bind_i64(sqlite3_stmt* stmt, const int i, const std::uint64_t value)
{
    if (value == 0 || value
            > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        || sqlite3_bind_int64(stmt, i, static_cast<sqlite3_int64>(value))
            != SQLITE_OK)
        throw std::runtime_error("trust floor bind generation failed");
}

std::string col_text(sqlite3_stmt* stmt, const int i)
{
    const unsigned char* p = sqlite3_column_text(stmt, i);
    const int n = sqlite3_column_bytes(stmt, i);
    if (!p || n < 0) throw std::runtime_error("missing trust floor text");
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<std::size_t>(n));
}

void verify_schema(sqlite3* db)
{
    {
        Statement s(db, "PRAGMA user_version");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || sqlite3_column_int(s.get(), 0)
                != local_goose_dialogue_responder_retry_trust_floor_schema
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("wrong trust floor schema version");
    }
    {
        Statement s(db, "PRAGMA journal_mode");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || col_text(s.get(), 0) != "delete"
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("unexpected trust floor journal mode");
    }
    {
        Statement s(db, "SELECT type,name,sql FROM sqlite_master "
                        "WHERE name NOT LIKE 'sqlite_%'");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || col_text(s.get(), 0) != "table"
            || col_text(s.get(), 1) != table_name
            || col_text(s.get(), 2) != schema_sql
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("untrusted trust floor schema");
    }
}

Record read_record(sqlite3* db)
{
    Statement s(db,
        "SELECT root_fingerprint,minimum_generation,last_transition_sha256 "
        "FROM local_goose_dialogue_responder_retry_trust_floor "
        "WHERE slot=1");
    if (sqlite3_step(s.get()) != SQLITE_ROW)
        throw std::runtime_error("missing trust floor record");
    Record result;
    result.root_fingerprint = col_text(s.get(), 0);
    const auto floor = sqlite3_column_int64(s.get(), 1);
    result.last_transition_sha256 = col_text(s.get(), 2);
    if (floor < 1
        || !lower_sha256(result.root_fingerprint)
        || !lower_sha256(result.last_transition_sha256)
        || sqlite3_step(s.get()) != SQLITE_DONE)
        throw std::runtime_error("invalid trust floor record");
    result.minimum_generation = static_cast<std::uint64_t>(floor);
    return result;
}

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit()
    {
        exec(db_, "COMMIT");
        committed_ = true;
    }
private:
    sqlite3* db_;
    bool committed_ = false;
};

} // namespace

LocalGooseDialogueResponderRetryTrustFloorLedger::
LocalGooseDialogueResponderRetryTrustFloorLedger(
    const std::string& path, const Mode mode, const std::string& initial_root)
{
    read_only_ = mode == Mode::existing_read_only;
    require_safe_parent(path);
    reject_companions(path);
    const bool creating = mode == Mode::create_new_isolated;
    if (creating) {
        if (!lower_sha256(initial_root))
            throw std::runtime_error("trust root has no independent fingerprint");
        create_exclusive(path);
    } else if (!initial_root.empty()) {
        throw std::runtime_error("existing trust floor cannot enroll new root");
    }
    const struct stat before = require_safe_file(path);
    try {
        sqlite3* opened = nullptr;
        const int flags =
            (read_only_ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE)
            | SQLITE_OPEN_FULLMUTEX;
        const int result = sqlite3_open_v2(path.c_str(), &opened, flags, nullptr);
        database_ = opened;
        if (result != SQLITE_OK || !database_)
            throw std::runtime_error("cannot open trust floor SQLite file");
        sqlite3_busy_timeout(database_, 3000);
        if (creating) {
            exec(database_, "PRAGMA journal_mode=DELETE");
            exec(database_, "PRAGMA synchronous=FULL");
            exec(database_, schema_sql);
            exec(database_, "PRAGMA user_version=1");
            const auto initial_evidence = sha256_hex(
                "gaudere-retry-trust-floor-bootstrap-not-enrollment.v1|" + initial_root);
            Statement s(database_,
                "INSERT INTO local_goose_dialogue_responder_retry_trust_floor"
                "(slot,root_fingerprint,minimum_generation,last_transition_sha256)"
                " VALUES(1,?1,1,?2)");
            bind_text(s.get(), 1, initial_root);
            bind_text(s.get(), 2, initial_evidence);
            if (sqlite3_step(s.get()) != SQLITE_DONE)
                throw std::runtime_error("cannot create initial inert trust floor");
        }
        const struct stat after = require_safe_file(path);
        if (before.st_dev != after.st_dev || before.st_ino != after.st_ino)
            throw std::runtime_error("trust floor file replaced during opening");
        verify_schema(database_);
        (void)read_record(database_);
    } catch (...) {
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderRetryTrustFloorLedger::
~LocalGooseDialogueResponderRetryTrustFloorLedger()
{
    if (database_) sqlite3_close(database_);
}

std::optional<Record>
LocalGooseDialogueResponderRetryTrustFloorLedger::inspect(
    const std::string& independently_trusted_root_fingerprint) const
{
    if (!database_ || !lower_sha256(independently_trusted_root_fingerprint))
        return std::nullopt;
    try {
        const auto state = read_record(database_);
        if (state.root_fingerprint != independently_trusted_root_fingerprint)
            return std::nullopt;
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

Code LocalGooseDialogueResponderRetryTrustFloorLedger::
advance_after_external_signature_review(
    const std::string& independently_trusted_root_fingerprint,
    const std::uint64_t expected_prior_generation,
    const std::uint64_t root_signed_new_generation,
    const std::string& externally_verified_snapshot_sha256)
{
    if (!database_ || read_only_) return Code::unavailable_or_commit_uncertain;
    if (!lower_sha256(independently_trusted_root_fingerprint)
        || !lower_sha256(externally_verified_snapshot_sha256)
        || expected_prior_generation < 1
        || root_signed_new_generation <= expected_prior_generation
        || root_signed_new_generation
            > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return Code::invalid;
    try {
        Transaction tx(database_);
        const auto current = read_record(database_);
        if (current.root_fingerprint != independently_trusted_root_fingerprint
            || current.minimum_generation != expected_prior_generation)
            return Code::stale_or_conflicting_floor;

        Statement s(database_,
            "UPDATE local_goose_dialogue_responder_retry_trust_floor "
            "SET minimum_generation=?1,last_transition_sha256=?2 "
            "WHERE slot=1 AND root_fingerprint=?3 AND minimum_generation=?4");
        bind_i64(s.get(), 1, root_signed_new_generation);
        bind_text(s.get(), 2, externally_verified_snapshot_sha256);
        bind_text(s.get(), 3, independently_trusted_root_fingerprint);
        bind_i64(s.get(), 4, expected_prior_generation);
        if (sqlite3_step(s.get()) != SQLITE_DONE
            || sqlite3_changes(database_) != 1)
            throw std::runtime_error("trust floor update uncertain");
        tx.commit();
        return Code::advanced_no_authority;
    } catch (...) {
        // A lost response may hide a completed COMMIT. Inspect durable state;
        // do NOT automatically retry or infer consent from the record.
        return Code::unavailable_or_commit_uncertain;
    }
}

} // namespace gaudere_agent
