#include "LocalGooseDialogueResponderRetryStore.hpp"

#include <nlohmann/json.hpp>
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
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_identifier_bytes = 128;
constexpr const char* lineage_prefix =
    "cognition.local-goose-dialogue.responder-retry-lineage.v1:";

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
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
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
                            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

void bind_int64(sqlite3* database,
                sqlite3_stmt* statement,
                const int index,
                const std::int64_t value)
{
    if (sqlite3_bind_int64(statement, index, value) != SQLITE_OK) {
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

bool safe_identifier(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_identifier_bytes) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_'
            || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& value, const char* prefix) noexcept
{
    const std::string p{prefix};
    return value.size() == p.size() + 64
        && value.compare(0, p.size(), p) == 0
        && lowercase_sha256(value.substr(p.size()));
}

void verify_owner_mode(const std::string& path)
{
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0
        || !S_ISREG(status.st_mode)
        || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "dialogue responder retry sidecar must be a current-user regular file mode 0600");
    }
}

void ensure_secure_file(const std::string& path)
{
    const int descriptor = open(
        path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
        close(descriptor);
        verify_owner_mode(path);
        return;
    }
    if (errno != EEXIST) {
        throw std::runtime_error("cannot securely create responder retry sidecar");
    }
    verify_owner_mode(path);
}

std::vector<std::string> user_table_names(sqlite3* database)
{
    Statement statement(
        database,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name");
    std::vector<std::string> out;
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
        out.push_back(text(statement.get(), 0));
    }
    return out;
}

void create_lineage_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_retry_lineage("
        "lineage_id TEXT PRIMARY KEY NOT NULL,"
        "request_id TEXT NOT NULL UNIQUE,"
        "consumer_id TEXT NOT NULL,"
        "completion_sequence INTEGER NOT NULL CHECK(completion_sequence > 0),"
        "event_id TEXT NOT NULL,"
        "retry_generation INTEGER NOT NULL CHECK(retry_generation = 1),"
        "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
        "document TEXT NOT NULL,"
        "UNIQUE(consumer_id,event_id),"
        "UNIQUE(consumer_id,completion_sequence));");
}

Json lineage_json(const LocalGooseDialogueResponderRetryLineage& lineage)
{
    return Json{
        {"lineage_id", lineage.lineage_id},
        {"request_id", lineage.request_id},
        {"consumer_id", lineage.consumer_id},
        {"thread_alias", lineage.thread_alias},
        {"completion_sequence", lineage.completion_sequence},
        {"event_id", lineage.event_id},
        {"result_sha256", lineage.result_sha256},
        {"thread_revision", lineage.thread_revision},
        {"task_id", lineage.task_id},
        {"observed_speaker_kind", lineage.observed_speaker_kind},
        {"observed_speaker_id", lineage.observed_speaker_id},
        {"retry_generation", lineage.retry_generation},
        {"historical_grant_id", lineage.historical_grant_id},
        {"historical_lease_id", lineage.historical_lease_id},
        {"historical_decision_id", lineage.historical_decision_id},
        {"historical_intent_id", lineage.historical_intent_id},
        {"historical_disposition_id", lineage.historical_disposition_id},
        {"historical_grant_terminal_reason",
         lineage.historical_grant_terminal_reason},
        {"historical_lease_terminal_reason",
         lineage.historical_lease_terminal_reason},
        {"historical_decision_terminal_reason",
         lineage.historical_decision_terminal_reason},
        {"historical_intent_terminal_reason",
         lineage.historical_intent_terminal_reason},
        {"historical_disposition_terminal_reason",
         lineage.historical_disposition_terminal_reason},
        {"created_at_ms", lineage.created_at_ms}
    };
}

LocalGooseDialogueResponderRetryLineage parse_lineage(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderRetryLineage out;
    out.lineage_id = j.at("lineage_id").get<std::string>();
    out.request_id = j.at("request_id").get<std::string>();
    out.consumer_id = j.at("consumer_id").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.completion_sequence = j.at("completion_sequence").get<std::uint64_t>();
    out.event_id = j.at("event_id").get<std::string>();
    out.result_sha256 = j.at("result_sha256").get<std::string>();
    out.thread_revision = j.at("thread_revision").get<std::uint64_t>();
    out.task_id = j.at("task_id").get<std::string>();
    out.observed_speaker_kind = j.at("observed_speaker_kind").get<std::string>();
    out.observed_speaker_id = j.at("observed_speaker_id").get<std::string>();
    out.retry_generation = j.at("retry_generation").get<std::uint32_t>();
    out.historical_grant_id = j.at("historical_grant_id").get<std::string>();
    out.historical_lease_id = j.at("historical_lease_id").get<std::string>();
    out.historical_decision_id = j.at("historical_decision_id").get<std::string>();
    out.historical_intent_id = j.at("historical_intent_id").get<std::string>();
    out.historical_disposition_id =
        j.at("historical_disposition_id").get<std::string>();
    out.historical_grant_terminal_reason =
        j.at("historical_grant_terminal_reason").get<std::string>();
    out.historical_lease_terminal_reason =
        j.at("historical_lease_terminal_reason").get<std::string>();
    out.historical_decision_terminal_reason =
        j.at("historical_decision_terminal_reason").get<std::string>();
    out.historical_intent_terminal_reason =
        j.at("historical_intent_terminal_reason").get<std::string>();
    out.historical_disposition_terminal_reason =
        j.at("historical_disposition_terminal_reason").get<std::string>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    if (!valid_local_goose_dialogue_responder_retry_lineage(out)) {
        throw std::runtime_error("non-canonical responder retry lineage row");
    }
    return out;
}

LocalGooseDialogueResponderRetryLineage read_lineage_row(sqlite3_stmt* statement)
{
    const auto row_request = text(statement, 0);
    const auto row_consumer = text(statement, 1);
    const auto row_sequence = sqlite3_column_int64(statement, 2);
    const auto row_event = text(statement, 3);
    const auto row_generation = sqlite3_column_int64(statement, 4);
    const auto row_created = sqlite3_column_int64(statement, 5);
    if (row_sequence <= 0
        || row_generation != local_goose_dialogue_responder_retry_generation
        || row_created < 0) {
        throw std::runtime_error("invalid responder retry lineage index columns");
    }
    auto out = parse_lineage(text(statement, 6));
    if (out.request_id != row_request || out.consumer_id != row_consumer
        || out.completion_sequence != static_cast<std::uint64_t>(row_sequence)
        || out.event_id != row_event
        || out.retry_generation != static_cast<std::uint32_t>(row_generation)
        || out.created_at_ms != row_created) {
        throw std::runtime_error("responder retry lineage index/document mismatch");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderRetryLineage> find_lineage(
    sqlite3* database, const std::string& lineage_id)
{
    Statement statement(database,
        "SELECT request_id,consumer_id,completion_sequence,event_id,"
        "retry_generation,created_at_ms,document "
        "FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE lineage_id=?1");
    bind_text(database, statement.get(), 1, lineage_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_lineage_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate responder retry lineage row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderRetryLineage> find_by_request(
    sqlite3* database, const std::string& request_id)
{
    Statement statement(database,
        "SELECT request_id,consumer_id,completion_sequence,event_id,"
        "retry_generation,created_at_ms,document "
        "FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE request_id=?1");
    bind_text(database, statement.get(), 1, request_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_lineage_row(statement.get());
}

std::optional<LocalGooseDialogueResponderRetryLineage> find_event(
    sqlite3* database,
    const std::string& consumer_id,
    const std::string& event_id)
{
    Statement statement(database,
        "SELECT request_id,consumer_id,completion_sequence,event_id,"
        "retry_generation,created_at_ms,document "
        "FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE consumer_id=?1 AND event_id=?2");
    bind_text(database, statement.get(), 1, consumer_id);
    bind_text(database, statement.get(), 2, event_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_lineage_row(statement.get());
}

std::optional<LocalGooseDialogueResponderRetryLineage> find_sequence(
    sqlite3* database,
    const std::string& consumer_id,
    const std::uint64_t completion_sequence)
{
    Statement statement(database,
        "SELECT request_id,consumer_id,completion_sequence,event_id,"
        "retry_generation,created_at_ms,document "
        "FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE consumer_id=?1 AND completion_sequence=?2");
    bind_text(database, statement.get(), 1, consumer_id);
    bind_int64(database, statement.get(), 2,
               static_cast<std::int64_t>(completion_sequence));
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_lineage_row(statement.get());
}

} // namespace

LocalGooseDialogueResponderRetryStore::LocalGooseDialogueResponderRetryStore(
    const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_) : "cannot open responder retry SQLite";
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        execute(database_, "PRAGMA journal_mode=DELETE;");
        execute(database_, "PRAGMA synchronous=FULL;");
        execute(database_, "PRAGMA busy_timeout=5000;");
        execute(database_, "PRAGMA foreign_keys=ON;");
        execute(database_, "BEGIN IMMEDIATE;");

        Statement version(database_, "PRAGMA user_version");
        if (sqlite3_step(version.get()) != SQLITE_ROW) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        const int schema = sqlite3_column_int(version.get(), 0);
        if (schema > local_goose_dialogue_responder_retry_sidecar_schema) {
            throw std::runtime_error("unsupported responder retry schema");
        }
        if (schema == 0) {
            if (!user_table_names(database_).empty()) {
                throw std::runtime_error(
                    "unversioned responder retry sidecar is not empty");
            }
            create_lineage_table(database_);
            execute(database_, "PRAGMA user_version=1;");
            schema_version_ = 1;
        } else if (schema == 1) {
            if (user_table_names(database_)
                != std::vector<std::string>{
                    "local_goose_dialogue_responder_retry_lineage"}) {
                throw std::runtime_error("responder retry v1 table set differs");
            }
            schema_version_ = 1;
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderRetryStore::~LocalGooseDialogueResponderRetryStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseDialogueResponderRetryLineage>
LocalGooseDialogueResponderRetryStore::find_lineage(
    const std::string& lineage_id) const
{
    if (!prefixed_sha256(lineage_id, lineage_prefix)) return std::nullopt;
    return ::gaudere_agent::find_lineage(database_, lineage_id);
}

std::optional<LocalGooseDialogueResponderRetryLineage>
LocalGooseDialogueResponderRetryStore::find_by_request(
    const std::string& request_id) const
{
    if (!safe_identifier(request_id)) return std::nullopt;
    return ::gaudere_agent::find_by_request(database_, request_id);
}

std::optional<LocalGooseDialogueResponderRetryLineage>
LocalGooseDialogueResponderRetryStore::find_for_event(
    const std::string& consumer_id,
    const std::string& event_id) const
{
    if (!safe_identifier(consumer_id)) return std::nullopt;
    return find_event(database_, consumer_id, event_id);
}

LocalGooseDialogueResponderRetryLineageWrite
LocalGooseDialogueResponderRetryStore::prepare_lineage(
    const LocalGooseDialogueResponderRetryLineage& lineage)
{
    LocalGooseDialogueResponderRetryLineageWrite out;
    if (!valid_local_goose_dialogue_responder_retry_lineage(lineage)) {
        out.detail = "invalid responder retry lineage";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");

        const auto existing = ::gaudere_agent::find_lineage(
            database_, lineage.lineage_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderRetryStoreResult::duplicate;
            out.lineage = existing;
            out.detail = "responder retry lineage already prepared";
            return out;
        }

        const auto by_request = ::gaudere_agent::find_by_request(
            database_, lineage.request_id);
        if (by_request) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderRetryStoreResult::conflict;
            out.lineage = by_request;
            out.detail = "responder retry request id conflicts";
            return out;
        }

        const auto event = find_event(
            database_, lineage.consumer_id, lineage.event_id);
        if (event) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderRetryStoreResult::conflict;
            out.lineage = event;
            out.detail = "responder retry event already has a lineage";
            return out;
        }

        const auto sequence = find_sequence(
            database_, lineage.consumer_id, lineage.completion_sequence);
        if (sequence) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderRetryStoreResult::conflict;
            out.lineage = sequence;
            out.detail = "responder retry sequence already has a lineage";
            return out;
        }

        Statement statement(database_,
            "INSERT INTO local_goose_dialogue_responder_retry_lineage("
            "lineage_id,request_id,consumer_id,completion_sequence,event_id,"
            "retry_generation,created_at_ms,document)"
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)");
        bind_text(database_, statement.get(), 1, lineage.lineage_id);
        bind_text(database_, statement.get(), 2, lineage.request_id);
        bind_text(database_, statement.get(), 3, lineage.consumer_id);
        bind_int64(database_, statement.get(), 4,
                   static_cast<std::int64_t>(lineage.completion_sequence));
        bind_text(database_, statement.get(), 5, lineage.event_id);
        bind_int64(database_, statement.get(), 6,
                   static_cast<std::int64_t>(lineage.retry_generation));
        bind_int64(database_, statement.get(), 7, lineage.created_at_ms);
        bind_text(database_, statement.get(), 8, lineage_json(lineage).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderRetryStoreResult::accepted;
        out.lineage = lineage;
        out.detail = "responder retry lineage prepared";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderRetryStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

} // namespace gaudere_agent
