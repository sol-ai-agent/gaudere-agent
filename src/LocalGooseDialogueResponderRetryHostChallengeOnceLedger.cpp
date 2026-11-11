#include "LocalGooseDialogueResponderRetryHostChallengeOnceLedger.hpp"

#include "Sha256.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryHostOnceCode;

bool lower_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    bool nonzero = false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool canonical_path(const std::string& path) noexcept
{
    if (path.size() < 3 || path.size() > 4096 || path[0] != '/'
        || path.find('\0') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto slash = path.find('/', start);
        const auto n =
            (slash == std::string::npos ? path.size() : slash) - start;
        if (!n || (n == 1 && path[start] == '.')
            || (n == 2 && path.compare(start, n, "..") == 0))
            return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}

void require_private_parent(const std::string& path)
{
    if (!canonical_path(path))
        throw std::runtime_error("invalid host once path");
    const auto split = path.find_last_of('/');
    const std::string parent =
        split == 0 ? "/" : path.substr(0, split);
    struct stat s {};
    if (lstat(parent.c_str(), &s) != 0 || !S_ISDIR(s.st_mode)
        || s.st_uid != geteuid() || (s.st_mode & 07777) != 0700)
        throw std::runtime_error("host once parent must be private 0700");
}

struct stat safe_file(const std::string& path)
{
    struct stat s {};
    if (lstat(path.c_str(), &s) != 0 || !S_ISREG(s.st_mode)
        || s.st_uid != geteuid() || (s.st_mode & 07777) != 0600
        || s.st_nlink != 1)
        throw std::runtime_error("host once ledger must exist regular owner 0600");
    return s;
}

void no_sqlite_companions(const std::string& path)
{
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        struct stat s {};
        if (lstat((path + suffix).c_str(), &s) == 0 || errno != ENOENT)
            throw std::runtime_error("host once SQLite sidecar forbidden");
    }
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error("host once SQL prepare failed");
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
        throw std::runtime_error("host once SQL operation uncertain");
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value)
{
    if (sqlite3_bind_text(stmt, index, value.data(),
                         static_cast<int>(value.size()), SQLITE_TRANSIENT)
        != SQLITE_OK)
        throw std::runtime_error("host once bind text failed");
}

void bind_i64(sqlite3_stmt* stmt, int index, std::uint64_t value)
{
    if (value == 0
        || value > static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max())
        || sqlite3_bind_int64(stmt, index,
                             static_cast<sqlite3_int64>(value)) != SQLITE_OK)
        throw std::runtime_error("host once bind epoch failed");
}

std::string column_text(sqlite3_stmt* stmt, int index)
{
    if (sqlite3_column_type(stmt, index) != SQLITE_TEXT)
        throw std::runtime_error("host once field must be TEXT");
    const unsigned char* p = sqlite3_column_text(stmt, index);
    const int n = sqlite3_column_bytes(stmt, index);
    if (!p || n < 0) throw std::runtime_error("host once missing text");
    return std::string(reinterpret_cast<const char*>(p),
                       static_cast<std::size_t>(n));
}

std::int64_t column_positive_i64(sqlite3_stmt* stmt, int index)
{
    if (sqlite3_column_type(stmt, index) != SQLITE_INTEGER)
        throw std::runtime_error("host once field must be INTEGER");
    const auto value = sqlite3_column_int64(stmt, index);
    if (value <= 0) throw std::runtime_error("host once non-positive value");
    return value;
}

void verify_schema(sqlite3* db)
{
    {
        Statement s(db, "PRAGMA user_version");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || sqlite3_column_int(s.get(), 0)
                != local_goose_dialogue_responder_retry_host_once_schema_version
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("host once schema version wrong");
    }
    {
        Statement s(db, "PRAGMA journal_mode");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || column_text(s.get(), 0) != "delete"
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("host once journal mode wrong");
    }
    {
        Statement s(db, "SELECT type,name,sql FROM sqlite_master "
                        "WHERE name NOT LIKE 'sqlite_%'");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || column_text(s.get(), 0) != "table"
            || column_text(s.get(), 1)
                != local_goose_dialogue_responder_retry_host_once_table
            || column_text(s.get(), 2)
                != local_goose_dialogue_responder_retry_host_once_schema_sql
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("host once unexpected schema");
    }
}

struct Row {
    std::string request;
    std::string context;
    std::uint64_t epoch = 0;
    std::int64_t expires = 0;
    bool consumed = false;
    std::string accepted;
};

std::optional<Row> read(sqlite3* db, const std::string& challenge)
{
    Statement s(db,
        "SELECT request_context_sha256,host_context_sha256,anchor_epoch,"
        "expires_ms,consumed,accepted_proof_sha256 "
        "FROM local_goose_dialogue_responder_retry_host_challenge_once "
        "WHERE challenge_sha256=?1");
    bind_text(s.get(), 1, challenge);
    const int status = sqlite3_step(s.get());
    if (status == SQLITE_DONE) return std::nullopt;
    if (status != SQLITE_ROW)
        throw std::runtime_error("host once record unreadable");

    Row result;
    result.request = column_text(s.get(), 0);
    result.context = column_text(s.get(), 1);
    result.epoch = static_cast<std::uint64_t>(column_positive_i64(s.get(), 2));
    result.expires = column_positive_i64(s.get(), 3);
    if (sqlite3_column_type(s.get(), 4) != SQLITE_INTEGER)
        throw std::runtime_error("host once consumed has wrong type");
    const auto consumed = sqlite3_column_int(s.get(), 4);
    if (consumed != 0 && consumed != 1)
        throw std::runtime_error("host once consumed invalid");
    result.consumed = consumed == 1;
    result.accepted = column_text(s.get(), 5);
    if (!lower_sha256(result.request) || !lower_sha256(result.context)
        || (result.consumed && !lower_sha256(result.accepted))
        || (!result.consumed && !result.accepted.empty())
        || sqlite3_step(s.get()) != SQLITE_DONE)
        throw std::runtime_error("host once corrupted row");
    return result;
}

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db)
    {
        exec(db_, "BEGIN IMMEDIATE");
    }
    ~Transaction()
    {
        if (!committed_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
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

LocalGooseDialogueResponderRetryHostChallengeOnceLedger::
LocalGooseDialogueResponderRetryHostChallengeOnceLedger(
    const std::string& path)
{
    require_private_parent(path);
    no_sqlite_companions(path);
    const auto before = safe_file(path);
    try {
        sqlite3* db = nullptr;
        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX
            | SQLITE_OPEN_NOFOLLOW; // No CREATE, never follow last symlink.
        const auto rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
        database_ = db;
        if (rc != SQLITE_OK || !database_)
            throw std::runtime_error("host once existing SQLite unavailable");
        sqlite3_busy_timeout(database_, 3000);
        exec(database_, "PRAGMA synchronous=FULL");
        const auto after = safe_file(path);
        if (before.st_dev != after.st_dev || before.st_ino != after.st_ino)
            throw std::runtime_error("host once file replaced during opening");
        verify_schema(database_);
    } catch (...) {
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderRetryHostChallengeOnceLedger::
~LocalGooseDialogueResponderRetryHostChallengeOnceLedger()
{
    if (database_) sqlite3_close(database_);
}

Code verify_and_mark_local_goose_dialogue_responder_retry_host_challenge_once(
    LocalGooseDialogueResponderRetryHostChallengeOnceLedger& ledger,
    const std::string& exact_v1_host_signed_receipt,
    const std::string& exact_v2_host_challenge_proof,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    const std::uint64_t independently_trusted_minimum_anchor_epoch,
    const std::string& independently_expected_challenge_sha256,
    const std::string& independently_expected_request_context_sha256,
    const std::int64_t trusted_now_ms)
{
    if (!ledger.database_)
        return Code::ledger_unavailable_or_commit_uncertain_no_authority;

    // Verify before taking the SQLite write lock, but ALWAYS compare the
    // challenge row inside BEGIN IMMEDIATE before a one-way CAS commit.
    if (inspect_local_goose_dialogue_responder_retry_host_challenge_proof(
            exact_v1_host_signed_receipt, exact_v2_host_challenge_proof,
            independently_pinned_host_signer_public_key,
            independently_pinned_host_signer_fingerprint,
            independently_expected_host_context_sha256,
            observed_anchor, independently_trusted_minimum_anchor_epoch,
            independently_expected_challenge_sha256,
            independently_expected_request_context_sha256,
            trusted_now_ms)
        != LocalGooseDialogueResponderRetryHostChallengeCode::
            challenge_bound_cryptographic_consistency_no_authority)
        return Code::invalid_signed_proof_no_authority;

    if (!lower_sha256(independently_expected_challenge_sha256)
        || !lower_sha256(independently_expected_request_context_sha256))
        return Code::invalid_signed_proof_no_authority;

    try {
        Transaction tx(ledger.database_);
        const auto state = read(
            ledger.database_, independently_expected_challenge_sha256);
        if (!state || state->consumed)
            return Code::missing_or_already_consumed_no_authority;
        if (state->request != independently_expected_request_context_sha256
            || state->context != independently_expected_host_context_sha256
            || state->epoch != observed_anchor.anchor_epoch
            || trusted_now_ms <= 0 || trusted_now_ms >= state->expires)
            return Code::mismatched_or_expired_issued_challenge_no_authority;

        Statement s(ledger.database_,
            "UPDATE local_goose_dialogue_responder_retry_host_challenge_once "
            "SET consumed=1,accepted_proof_sha256=?1 "
            "WHERE challenge_sha256=?2 AND consumed=0 "
            "AND request_context_sha256=?3 AND host_context_sha256=?4 "
            "AND anchor_epoch=?5 AND expires_ms>?6");
        bind_text(s.get(), 1, sha256_hex(exact_v2_host_challenge_proof));
        bind_text(s.get(), 2, independently_expected_challenge_sha256);
        bind_text(s.get(), 3, independently_expected_request_context_sha256);
        bind_text(s.get(), 4, independently_expected_host_context_sha256);
        bind_i64(s.get(), 5, observed_anchor.anchor_epoch);
        if (sqlite3_bind_int64(s.get(), 6,
                static_cast<sqlite3_int64>(trusted_now_ms)) != SQLITE_OK
            || sqlite3_step(s.get()) != SQLITE_DONE
            || sqlite3_changes(ledger.database_) != 1)
            throw std::runtime_error("host once update uncertain");
        tx.commit();
        return Code::verified_challenge_marked_consumed_no_authority;
    } catch (...) {
        // Commit ambiguity MUST NOT be treated as a successful grant.
        // Do not retry automatically: inspect under explicit recovery gate.
        return Code::ledger_unavailable_or_commit_uncertain_no_authority;
    }
}

} // namespace gaudere_agent
