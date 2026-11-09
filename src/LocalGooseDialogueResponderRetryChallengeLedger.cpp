#include "LocalGooseDialogueResponderRetryChallengeLedger.hpp"

#include "Sha256.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

using Reservation = LocalGooseDialogueResponderRetryChallengeReservation;
using Record = LocalGooseDialogueResponderRetryChallengeRecord;
using State = LocalGooseDialogueResponderRetryChallengeState;
using Mode = LocalGooseDialogueResponderRetryChallengeLedgerOpenMode;
using Code = LocalGooseDialogueResponderRetryChallengeLedgerCode;

constexpr const char* table_name =
    "local_goose_dialogue_responder_retry_challenge_reservation";
constexpr const char* schema_sql =
    "CREATE TABLE local_goose_dialogue_responder_retry_challenge_reservation("
    "reservation_id TEXT PRIMARY KEY NOT NULL,"
    "request_id TEXT NOT NULL UNIQUE,"
    "challenge_id TEXT NOT NULL UNIQUE,"
    "proposal_id TEXT NOT NULL,"
    "scope_id TEXT NOT NULL,"
    "lineage_id TEXT NOT NULL,"
    "nonce_sha256 TEXT NOT NULL UNIQUE,"
    "retry_generation INTEGER NOT NULL CHECK(retry_generation=1),"
    "issued_at_ms INTEGER NOT NULL CHECK(issued_at_ms>=0),"
    "expires_at_ms INTEGER NOT NULL CHECK(expires_at_ms>issued_at_ms),"
    "state INTEGER NOT NULL CHECK(state IN (0,1,2)),"
    "evidence_sha256 TEXT,"
    "UNIQUE(scope_id,retry_generation),"
    "CHECK((state=0 AND evidence_sha256 IS NULL)"
    " OR (state=1 AND evidence_sha256 IS NOT NULL)"
    " OR (state=2 AND evidence_sha256 IS NULL)))";
constexpr const char* columns =
    "reservation_id,request_id,challenge_id,proposal_id,scope_id,lineage_id,"
    "nonce_sha256,retry_generation,issued_at_ms,expires_at_ms,state,evidence_sha256";

class Statement {
public:
    Statement(sqlite3* db, const std::string& query) : db_(db)
    {
        if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return stmt_; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void exec(sqlite3* db, const char* command)
{
    if (sqlite3_exec(db, command, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}

void bind_text(sqlite3_stmt* stmt, int col, const std::string& value)
{
    if (sqlite3_bind_text(stmt, col, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("retry challenge bind failed");
}

void bind_i64(sqlite3_stmt* stmt, int col, std::int64_t value)
{
    if (sqlite3_bind_int64(stmt, col, value) != SQLITE_OK)
        throw std::runtime_error("retry challenge integer bind failed");
}

std::string col_text(sqlite3_stmt* s, int index)
{
    const auto* bytes = sqlite3_column_text(s, index);
    const int count = sqlite3_column_bytes(s, index);
    if (!bytes || count < 0) return {};
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(count));
}

bool lower_sha256(const std::string& text) noexcept
{
    if (text.size() != 64) return false;
    for (const unsigned char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool prefixed_sha256(const std::string& id, const char* prefix) noexcept
{
    const std::string p(prefix);
    return id.size() == p.size() + 64
        && id.compare(0, p.size(), p) == 0
        && lower_sha256(id.substr(p.size()));
}

bool safe_request(const std::string& s) noexcept
{
    if (s.empty() || s.size() > 128) return false;
    for (const unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '_' || c == '.'
              || c == ':' || c == '-')) return false;
    return true;
}

void append(std::string& bytes, const std::string& text)
{
    bytes += std::to_string(text.size()) + ":" + text + "|";
}

void append(std::string& bytes, std::int64_t n)
{
    bytes += std::to_string(n) + "|";
}

std::string reservation_id(const Reservation& r)
{
    std::string s =
        "gaudere.cognition.local-goose-dialogue-responder-retry-challenge-reservation.v1|";
    append(s, r.request_id);
    append(s, r.challenge_id);
    append(s, r.proposal_id);
    append(s, r.scope_id);
    append(s, r.lineage_id);
    append(s, r.nonce_sha256);
    append(s, static_cast<std::int64_t>(r.retry_generation));
    append(s, r.issued_at_ms);
    append(s, r.expires_at_ms);
    return std::string(local_goose_dialogue_responder_retry_reservation_prefix)
        + sha256_hex(s);
}

bool same(const Reservation& a, const Reservation& b)
{
    return a.reservation_id == b.reservation_id
        && a.request_id == b.request_id
        && a.challenge_id == b.challenge_id
        && a.proposal_id == b.proposal_id
        && a.scope_id == b.scope_id
        && a.lineage_id == b.lineage_id
        && a.nonce_sha256 == b.nonce_sha256
        && a.retry_generation == b.retry_generation
        && a.issued_at_ms == b.issued_at_ms
        && a.expires_at_ms == b.expires_at_ms;
}

void require_file(const std::string& path)
{
    struct stat s {};
    if (lstat(path.c_str(), &s) != 0 || !S_ISREG(s.st_mode)
        || s.st_uid != geteuid() || (s.st_mode & 0777) != 0600)
        throw std::runtime_error("retry challenge ledger requires owner regular 0600 file");
}

void reject_companions(const std::string& path)
{
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        struct stat s {};
        const std::string candidate = path + suffix;
        if (lstat(candidate.c_str(), &s) == 0 || errno != ENOENT)
            throw std::runtime_error("retry challenge journal/companion present");
    }
}

void create_exclusive(const std::string& path)
{
    const int fd = open(path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("retry challenge ledger path already exists or unsafe");
    close(fd);
    require_file(path);
}

void verify_schema(sqlite3* db)
{
    {
        Statement s(db, "PRAGMA user_version");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || sqlite3_column_int(s.get(), 0)
                != local_goose_dialogue_responder_retry_challenge_ledger_schema
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("unexpected retry challenge schema version");
    }
    {
        Statement s(db, "PRAGMA journal_mode");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || col_text(s.get(), 0) != "delete"
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("retry challenge ledger must use DELETE journal");
    }
    {
        Statement s(db, "SELECT type,name,sql FROM sqlite_master "
                        "WHERE name NOT LIKE 'sqlite_%'");
        if (sqlite3_step(s.get()) != SQLITE_ROW
            || col_text(s.get(), 0) != "table"
            || col_text(s.get(), 1) != table_name
            || col_text(s.get(), 2) != schema_sql
            || sqlite3_step(s.get()) != SQLITE_DONE)
            throw std::runtime_error("unexpected retry challenge table/DDL");
    }
}

Record decode(sqlite3_stmt* stmt)
{
    Record record;
    auto& r = record.reservation;
    r.reservation_id = col_text(stmt, 0);
    r.request_id = col_text(stmt, 1);
    r.challenge_id = col_text(stmt, 2);
    r.proposal_id = col_text(stmt, 3);
    r.scope_id = col_text(stmt, 4);
    r.lineage_id = col_text(stmt, 5);
    r.nonce_sha256 = col_text(stmt, 6);
    const auto gen = sqlite3_column_int64(stmt, 7);
    r.issued_at_ms = sqlite3_column_int64(stmt, 8);
    r.expires_at_ms = sqlite3_column_int64(stmt, 9);
    const auto state = sqlite3_column_int64(stmt, 10);
    if (gen != 1 || state < 0 || state > 2)
        throw std::runtime_error("invalid generation or state in retry challenge row");
    r.retry_generation = 1;
    record.state = static_cast<State>(state);
    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL)
        record.evidence_sha256 = col_text(stmt, 11);
    if (!valid_local_goose_dialogue_responder_retry_challenge_reservation(r)
        || (record.state == State::burned)
            != record.evidence_sha256.has_value()
        || (record.evidence_sha256 && !lower_sha256(*record.evidence_sha256)))
        throw std::runtime_error("noncanonical retry challenge record");
    return record;
}

std::optional<Record> find_by(sqlite3* db, const char* column, const std::string& id)
{
    const std::string query = std::string("SELECT ") + columns
        + " FROM " + table_name + " WHERE " + column + "=?1";
    Statement s(db, query);
    bind_text(s.get(), 1, id);
    const int rc = sqlite3_step(s.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    if (rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    auto result = decode(s.get());
    if (sqlite3_step(s.get()) != SQLITE_DONE)
        throw std::runtime_error("ambiguous retry challenge lookup");
    return result;
}

struct Transaction {
    explicit Transaction(sqlite3* db) : db(db) { exec(db, "BEGIN IMMEDIATE"); }
    ~Transaction()
    {
        if (!committed) sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit()
    {
        exec(db, "COMMIT");
        committed = true;
    }
    sqlite3* db;
    bool committed = false;
};

} // namespace

std::optional<Reservation>
make_local_goose_dialogue_responder_retry_challenge_reservation(
    const std::string& request_id,
    const std::string& challenge_id,
    const std::string& proposal_id,
    const std::string& scope_id,
    const std::string& lineage_id,
    const std::string& nonce_sha256,
    const std::int64_t issued_at_ms,
    const std::int64_t expires_at_ms)
{
    Reservation r;
    r.request_id = request_id;
    r.challenge_id = challenge_id;
    r.proposal_id = proposal_id;
    r.scope_id = scope_id;
    r.lineage_id = lineage_id;
    r.nonce_sha256 = nonce_sha256;
    r.retry_generation = 1;
    r.issued_at_ms = issued_at_ms;
    r.expires_at_ms = expires_at_ms;
    if (!safe_request(r.request_id)
        || !prefixed_sha256(r.challenge_id,
            "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:")
        || !prefixed_sha256(r.proposal_id,
            "cognition.local-goose-dialogue.responder-retry-grant-proposal.v1:")
        || !prefixed_sha256(r.scope_id,
            "cognition.local-goose-dialogue.responder-retry-scope.v1:")
        || !prefixed_sha256(r.lineage_id,
            "cognition.local-goose-dialogue.responder-retry-lineage.v1:")
        || !lower_sha256(r.nonce_sha256)
        || r.issued_at_ms < 0 || r.expires_at_ms <= r.issued_at_ms
        || r.expires_at_ms - r.issued_at_ms > 300000)
        return std::nullopt;
    r.reservation_id = reservation_id(r);
    return valid_local_goose_dialogue_responder_retry_challenge_reservation(r)
        ? std::optional<Reservation>{r} : std::nullopt;
}

bool valid_local_goose_dialogue_responder_retry_challenge_reservation(
    const Reservation& r) noexcept
{
    if (!prefixed_sha256(
            r.reservation_id,
            local_goose_dialogue_responder_retry_reservation_prefix)
        || !safe_request(r.request_id)
        || !prefixed_sha256(r.challenge_id,
            "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:")
        || !prefixed_sha256(r.proposal_id,
            "cognition.local-goose-dialogue.responder-retry-grant-proposal.v1:")
        || !prefixed_sha256(r.scope_id,
            "cognition.local-goose-dialogue.responder-retry-scope.v1:")
        || !prefixed_sha256(r.lineage_id,
            "cognition.local-goose-dialogue.responder-retry-lineage.v1:")
        || !lower_sha256(r.nonce_sha256)
        || r.retry_generation != 1
        || r.issued_at_ms < 0
        || r.expires_at_ms <= r.issued_at_ms
        || r.expires_at_ms - r.issued_at_ms > 300000)
        return false;
    try { return r.reservation_id == reservation_id(r); }
    catch (...) { return false; }
}

LocalGooseDialogueResponderRetryChallengeLedger::
LocalGooseDialogueResponderRetryChallengeLedger(
    const std::string& path, const Mode mode)
{
    if (mode != Mode::create_new_isolated
        && mode != Mode::existing_read_only
        && mode != Mode::existing_writer)
        throw std::invalid_argument("invalid retry challenge ledger open mode");
    const bool fresh = mode == Mode::create_new_isolated;
    read_only_ = mode == Mode::existing_read_only;
    if (fresh) create_exclusive(path);
    else require_file(path);
    reject_companions(path);
    try {
        const int flags = SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW
            | (read_only_ ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE);
        if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot open retry challenge ledger");
        require_file(path);
        if (sqlite3_db_readonly(database_, "main") != (read_only_ ? 1 : 0))
            throw std::runtime_error("retry challenge ledger read-only mismatch");
        if (!read_only_) exec(database_, "PRAGMA synchronous=FULL");
        if (fresh) {
            exec(database_, "PRAGMA journal_mode=DELETE");
            exec(database_, schema_sql);
            exec(database_, "PRAGMA user_version=1");
        }
        verify_schema(database_);
        schema_version_ = local_goose_dialogue_responder_retry_challenge_ledger_schema;
    } catch (...) {
        if (database_) sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderRetryChallengeLedger::
~LocalGooseDialogueResponderRetryChallengeLedger()
{
    if (database_) sqlite3_close(database_);
}

std::optional<Record>
LocalGooseDialogueResponderRetryChallengeLedger::find_challenge(
    const std::string& challenge_id) const
{
    return find_by(database_, "challenge_id", challenge_id);
}

Code LocalGooseDialogueResponderRetryChallengeLedger::reserve(
    const Reservation& reservation)
{
    if (read_only_) return Code::unavailable;
    if (!valid_local_goose_dialogue_responder_retry_challenge_reservation(
            reservation))
        return Code::invalid;
    try {
        Transaction tx(database_);
        const auto by_challenge = find_by(
            database_, "challenge_id", reservation.challenge_id);
        const auto by_request = find_by(
            database_, "request_id", reservation.request_id);
        const auto by_nonce = find_by(
            database_, "nonce_sha256", reservation.nonce_sha256);
        const auto by_scope = find_by(
            database_, "scope_id", reservation.scope_id);
        if (by_challenge || by_request || by_nonce || by_scope) {
            const bool same_reserved = by_challenge && by_request
                && by_nonce && by_scope
                && same(by_challenge->reservation, reservation)
                && same(by_request->reservation, reservation)
                && same(by_nonce->reservation, reservation)
                && same(by_scope->reservation, reservation)
                && by_challenge->state == State::reserved
                && by_request->state == State::reserved
                && by_nonce->state == State::reserved
                && by_scope->state == State::reserved;
            tx.commit();
            return same_reserved ? Code::exact_duplicate_reserved : Code::conflict;
        }
        const std::string sql = std::string("INSERT INTO ") + table_name
            + "(" + columns + ") VALUES("
              "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,NULL)";
        Statement s(database_, sql);
        bind_text(s.get(), 1, reservation.reservation_id);
        bind_text(s.get(), 2, reservation.request_id);
        bind_text(s.get(), 3, reservation.challenge_id);
        bind_text(s.get(), 4, reservation.proposal_id);
        bind_text(s.get(), 5, reservation.scope_id);
        bind_text(s.get(), 6, reservation.lineage_id);
        bind_text(s.get(), 7, reservation.nonce_sha256);
        bind_i64(s.get(), 8, reservation.retry_generation);
        bind_i64(s.get(), 9, reservation.issued_at_ms);
        bind_i64(s.get(), 10, reservation.expires_at_ms);
        const int rc = sqlite3_step(s.get());
        if (rc != SQLITE_DONE) {
            if ((rc & 0xff) == SQLITE_CONSTRAINT) return Code::conflict;
            return Code::unavailable;
        }
        tx.commit();
        return Code::accepted;
    } catch (...) { return Code::unavailable; }
}

Code LocalGooseDialogueResponderRetryChallengeLedger::burn(
    const std::string& challenge_id,
    const std::string& request_id,
    const std::string& evidence_sha256)
{
    if (read_only_) return Code::unavailable;
    if (!prefixed_sha256(challenge_id,
            "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:")
        || !safe_request(request_id) || !lower_sha256(evidence_sha256))
        return Code::invalid;
    try {
        Transaction tx(database_);
        const auto record = find_challenge(challenge_id);
        if (!record || record->reservation.request_id != request_id
            || record->state != State::reserved) {
            tx.commit();
            return Code::conflict;
        }
        const std::string sql = std::string("UPDATE ") + table_name
            + " SET state=1,evidence_sha256=?1"
              " WHERE challenge_id=?2 AND request_id=?3 AND state=0";
        Statement s(database_, sql);
        bind_text(s.get(), 1, evidence_sha256);
        bind_text(s.get(), 2, challenge_id);
        bind_text(s.get(), 3, request_id);
        if (sqlite3_step(s.get()) != SQLITE_DONE
            || sqlite3_changes(database_) != 1)
            return Code::unavailable;
        tx.commit();
        return Code::accepted;
    } catch (...) { return Code::unavailable; }
}

Code LocalGooseDialogueResponderRetryChallengeLedger::mark_uncertain(
    const std::string& challenge_id,
    const std::string& request_id)
{
    if (read_only_) return Code::unavailable;
    if (!prefixed_sha256(challenge_id,
            "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:")
        || !safe_request(request_id))
        return Code::invalid;
    try {
        Transaction tx(database_);
        const auto record = find_challenge(challenge_id);
        if (!record || record->reservation.request_id != request_id
            || record->state != State::reserved) {
            tx.commit();
            return Code::conflict;
        }
        const std::string sql = std::string("UPDATE ") + table_name
            + " SET state=2 WHERE challenge_id=?1 AND request_id=?2 AND state=0";
        Statement s(database_, sql);
        bind_text(s.get(), 1, challenge_id);
        bind_text(s.get(), 2, request_id);
        if (sqlite3_step(s.get()) != SQLITE_DONE
            || sqlite3_changes(database_) != 1)
            return Code::unavailable;
        tx.commit();
        return Code::accepted;
    } catch (...) { return Code::unavailable; }
}

} // namespace gaudere_agent
