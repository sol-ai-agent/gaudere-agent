#include "LocalGooseDialogueResponderStore.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
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
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_identifier_bytes = 128;
constexpr std::size_t max_reason_bytes = 1024;
constexpr const char* intent_prefix =
    "cognition.local-goose-dialogue.responder-intent.v1:";
constexpr const char* disposition_prefix =
    "cognition.local-goose-dialogue.responder-disposition.v1:";
constexpr const char* completion_event_prefix =
    "cognition.local-goose-dialogue.completion.v1:";
constexpr const char* v3_prefix = "cognition.local-goose-dialogue.v3:";

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

void bind_null(sqlite3* database, sqlite3_stmt* statement, const int index)
{
    if (sqlite3_bind_null(statement, index) != SQLITE_OK) {
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

void bind_uint64(sqlite3* database,
                 sqlite3_stmt* statement,
                 const int index,
                 const std::uint64_t value)
{
    if (value > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("responder value exceeds SQLite range");
    }
    bind_int64(database, statement, index, static_cast<std::int64_t>(value));
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
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return text(statement, column);
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

bool exact_table_set(sqlite3* database,
                     std::initializer_list<const char*> expected)
{
    std::vector<std::string> names;
    names.reserve(expected.size());
    for (const char* name : expected) names.emplace_back(name);
    std::sort(names.begin(), names.end());
    return user_table_names(database) == names;
}

void verify_owner_mode(const std::string& path)
{
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0
        || !S_ISREG(status.st_mode)
        || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "dialogue responder sidecar must be a current-user regular file mode 0600");
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
        throw std::runtime_error("cannot securely create dialogue responder sidecar");
    }
    verify_owner_mode(path);
}

void create_lease_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_lease("
        "lease_id TEXT PRIMARY KEY NOT NULL,"
        "thread_alias TEXT NOT NULL,"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
        "turns_committed INTEGER NOT NULL CHECK(turns_committed >= 0),"
        "document TEXT NOT NULL);");
    execute(database,
        "CREATE UNIQUE INDEX local_goose_dialogue_responder_one_active "
        "ON local_goose_dialogue_responder_lease(thread_alias) "
        "WHERE state=0;");
}

void create_intent_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_intent("
        "intent_id TEXT PRIMARY KEY NOT NULL,"
        "lease_id TEXT NOT NULL,"
        "event_id TEXT NOT NULL,"
        "request_id TEXT NOT NULL UNIQUE,"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),"
        "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
        "document TEXT NOT NULL,"
        "UNIQUE(lease_id,event_id),"
        "FOREIGN KEY(lease_id) REFERENCES "
        "local_goose_dialogue_responder_lease(lease_id));");
}

void create_disposition_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_disposition("
        "disposition_id TEXT PRIMARY KEY NOT NULL,"
        "consumer_id TEXT NOT NULL,"
        "completion_sequence INTEGER NOT NULL CHECK(completion_sequence > 0),"
        "event_id TEXT NOT NULL,"
        "kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 3),"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
        "intent_id TEXT UNIQUE,"
        "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
        "document TEXT NOT NULL,"
        "UNIQUE(consumer_id,completion_sequence),"
        "UNIQUE(consumer_id,event_id),"
        "FOREIGN KEY(intent_id) REFERENCES "
        "local_goose_dialogue_responder_intent(intent_id));");
}

Json lease_json(const LocalGooseDialogueResponderLease& lease)
{
    Json out{
        {"lease_id", lease.lease_id},
        {"thread_alias", lease.thread_alias},
        {"speaker_kind", lease.speaker_kind},
        {"speaker_id", lease.speaker_id},
        {"allowed_message_kinds", lease.allowed_message_kinds},
        {"purpose", lease.purpose},
        {"max_system_turns", lease.max_system_turns},
        {"turns_committed", lease.turns_committed},
        {"issued_at_ms", lease.issued_at_ms},
        {"expires_at_ms", lease.expires_at_ms},
        {"min_interval_ms", lease.min_interval_ms},
        {"next_eligible_at_ms", lease.next_eligible_at_ms},
        {"state", static_cast<unsigned>(lease.state)},
        {"terminal_reason", lease.terminal_reason}
    };
    if (lease.terminal_at_ms) {
        out["terminal_at_ms"] = *lease.terminal_at_ms;
    } else {
        out["terminal_at_ms"] = nullptr;
    }
    return out;
}

Json intent_json(const LocalGooseDialogueResponderIntent& intent)
{
    Json out{
        {"intent_id", intent.intent_id},
        {"lease_id", intent.lease_id},
        {"thread_alias", intent.thread_alias},
        {"completion_sequence", intent.completion_sequence},
        {"event_id", intent.event_id},
        {"result_sha256", intent.result_sha256},
        {"triggering_task_id", intent.triggering_task_id},
        {"expected_thread_revision", intent.expected_thread_revision},
        {"expected_head_task_id", intent.expected_head_task_id},
        {"speaker_kind", intent.speaker_kind},
        {"speaker_id", intent.speaker_id},
        {"message_kind", intent.message_kind},
        {"message", intent.message},
        {"request_id", intent.request_id},
        {"created_at_ms", intent.created_at_ms},
        {"expires_at_ms", intent.expires_at_ms},
        {"state", static_cast<unsigned>(intent.state)},
        {"submitted_task_id", intent.submitted_task_id},
        {"terminal_reason", intent.terminal_reason}
    };
    if (intent.submitted_thread_revision) {
        out["submitted_thread_revision"] = *intent.submitted_thread_revision;
    } else {
        out["submitted_thread_revision"] = nullptr;
    }
    if (intent.committed_at_ms) {
        out["committed_at_ms"] = *intent.committed_at_ms;
    } else {
        out["committed_at_ms"] = nullptr;
    }
    if (intent.completed_at_ms) {
        out["completed_at_ms"] = *intent.completed_at_ms;
    } else {
        out["completed_at_ms"] = nullptr;
    }
    return out;
}

Json disposition_json(const LocalGooseDialogueResponderDisposition& disposition)
{
    Json out{
        {"disposition_id", disposition.disposition_id},
        {"consumer_id", disposition.consumer_id},
        {"thread_alias", disposition.thread_alias},
        {"completion_sequence", disposition.completion_sequence},
        {"event_id", disposition.event_id},
        {"result_sha256", disposition.result_sha256},
        {"thread_revision", disposition.thread_revision},
        {"task_id", disposition.task_id},
        {"speaker_kind", disposition.speaker_kind},
        {"speaker_id", disposition.speaker_id},
        {"kind", static_cast<unsigned>(disposition.kind)},
        {"authority_source", disposition.authority_source},
        {"created_at_ms", disposition.created_at_ms},
        {"state", static_cast<unsigned>(disposition.state)},
        {"terminal_reason", disposition.terminal_reason}
    };
    if (disposition.intent_id) {
        out["intent_id"] = *disposition.intent_id;
    } else {
        out["intent_id"] = nullptr;
    }
    if (disposition.bound_at_ms) {
        out["bound_at_ms"] = *disposition.bound_at_ms;
    } else {
        out["bound_at_ms"] = nullptr;
    }
    if (disposition.reconciled_at_ms) {
        out["reconciled_at_ms"] = *disposition.reconciled_at_ms;
    } else {
        out["reconciled_at_ms"] = nullptr;
    }
    if (disposition.terminal_at_ms) {
        out["terminal_at_ms"] = *disposition.terminal_at_ms;
    } else {
        out["terminal_at_ms"] = nullptr;
    }
    return out;
}

LocalGooseDialogueResponderLease parse_lease(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderLease out;
    out.lease_id = j.at("lease_id").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.speaker_kind = j.at("speaker_kind").get<std::string>();
    out.speaker_id = j.at("speaker_id").get<std::string>();
    out.allowed_message_kinds = j.at("allowed_message_kinds").get<std::uint32_t>();
    out.purpose = j.at("purpose").get<std::string>();
    out.max_system_turns = j.at("max_system_turns").get<std::uint64_t>();
    out.turns_committed = j.at("turns_committed").get<std::uint64_t>();
    out.issued_at_ms = j.at("issued_at_ms").get<std::int64_t>();
    out.expires_at_ms = j.at("expires_at_ms").get<std::int64_t>();
    out.min_interval_ms = j.at("min_interval_ms").get<std::int64_t>();
    out.next_eligible_at_ms = j.at("next_eligible_at_ms").get<std::int64_t>();
    out.state = static_cast<LocalGooseDialogueResponderLeaseState>(
        j.at("state").get<unsigned>());
    out.terminal_reason = j.at("terminal_reason").get<std::string>();
    if (!j.at("terminal_at_ms").is_null()) {
        out.terminal_at_ms = j.at("terminal_at_ms").get<std::int64_t>();
    }
    if (!valid_local_goose_dialogue_responder_lease(out)) {
        throw std::runtime_error("non-canonical dialogue responder lease row");
    }
    return out;
}

LocalGooseDialogueResponderIntent parse_intent(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderIntent out;
    out.intent_id = j.at("intent_id").get<std::string>();
    out.lease_id = j.at("lease_id").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.completion_sequence = j.at("completion_sequence").get<std::uint64_t>();
    out.event_id = j.at("event_id").get<std::string>();
    out.result_sha256 = j.at("result_sha256").get<std::string>();
    out.triggering_task_id = j.at("triggering_task_id").get<std::string>();
    out.expected_thread_revision =
        j.at("expected_thread_revision").get<std::uint64_t>();
    out.expected_head_task_id = j.at("expected_head_task_id").get<std::string>();
    out.speaker_kind = j.at("speaker_kind").get<std::string>();
    out.speaker_id = j.at("speaker_id").get<std::string>();
    out.message_kind = j.at("message_kind").get<std::string>();
    out.message = j.at("message").get<std::string>();
    out.request_id = j.at("request_id").get<std::string>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    out.expires_at_ms = j.at("expires_at_ms").get<std::int64_t>();
    out.state = static_cast<LocalGooseDialogueResponderIntentState>(
        j.at("state").get<unsigned>());
    out.submitted_task_id = j.at("submitted_task_id").get<std::string>();
    if (!j.at("submitted_thread_revision").is_null()) {
        out.submitted_thread_revision =
            j.at("submitted_thread_revision").get<std::uint64_t>();
    }
    if (!j.at("committed_at_ms").is_null()) {
        out.committed_at_ms = j.at("committed_at_ms").get<std::int64_t>();
    }
    if (!j.at("completed_at_ms").is_null()) {
        out.completed_at_ms = j.at("completed_at_ms").get<std::int64_t>();
    }
    out.terminal_reason = j.at("terminal_reason").get<std::string>();
    if (!valid_local_goose_dialogue_responder_intent(out)) {
        throw std::runtime_error("non-canonical dialogue responder intent row");
    }
    return out;
}

LocalGooseDialogueResponderDisposition parse_disposition(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderDisposition out;
    out.disposition_id = j.at("disposition_id").get<std::string>();
    out.consumer_id = j.at("consumer_id").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.completion_sequence = j.at("completion_sequence").get<std::uint64_t>();
    out.event_id = j.at("event_id").get<std::string>();
    out.result_sha256 = j.at("result_sha256").get<std::string>();
    out.thread_revision = j.at("thread_revision").get<std::uint64_t>();
    out.task_id = j.at("task_id").get<std::string>();
    out.speaker_kind = j.at("speaker_kind").get<std::string>();
    out.speaker_id = j.at("speaker_id").get<std::string>();
    out.kind = static_cast<LocalGooseDialogueResponderDispositionKind>(
        j.at("kind").get<unsigned>());
    out.authority_source = j.at("authority_source").get<std::string>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    out.state = static_cast<LocalGooseDialogueResponderDispositionState>(
        j.at("state").get<unsigned>());
    if (!j.at("intent_id").is_null()) {
        out.intent_id = j.at("intent_id").get<std::string>();
    }
    if (!j.at("bound_at_ms").is_null()) {
        out.bound_at_ms = j.at("bound_at_ms").get<std::int64_t>();
    }
    if (!j.at("reconciled_at_ms").is_null()) {
        out.reconciled_at_ms = j.at("reconciled_at_ms").get<std::int64_t>();
    }
    out.terminal_reason = j.at("terminal_reason").get<std::string>();
    if (!j.at("terminal_at_ms").is_null()) {
        out.terminal_at_ms = j.at("terminal_at_ms").get<std::int64_t>();
    }
    if (!valid_local_goose_dialogue_responder_disposition(out)) {
        throw std::runtime_error("non-canonical dialogue responder disposition row");
    }
    return out;
}

LocalGooseDialogueResponderLease read_lease_row(sqlite3_stmt* statement)
{
    const auto row_thread = text(statement, 0);
    const auto row_state = sqlite3_column_int64(statement, 1);
    const auto row_turns = sqlite3_column_int64(statement, 2);
    if (row_state < 0 || row_turns < 0) {
        throw std::runtime_error("invalid dialogue responder lease index columns");
    }
    auto out = parse_lease(text(statement, 3));
    if (out.thread_alias != row_thread
        || static_cast<std::int64_t>(out.state) != row_state
        || out.turns_committed != static_cast<std::uint64_t>(row_turns)) {
        throw std::runtime_error(
            "dialogue responder lease index/document mismatch");
    }
    return out;
}

LocalGooseDialogueResponderIntent read_intent_row(sqlite3_stmt* statement)
{
    const auto row_lease = text(statement, 0);
    const auto row_event = text(statement, 1);
    const auto row_request = text(statement, 2);
    const auto row_state = sqlite3_column_int64(statement, 3);
    const auto row_created = sqlite3_column_int64(statement, 4);
    if (row_state < 0 || row_created < 0) {
        throw std::runtime_error("invalid dialogue responder intent index columns");
    }
    auto out = parse_intent(text(statement, 5));
    if (out.lease_id != row_lease
        || out.event_id != row_event
        || out.request_id != row_request
        || static_cast<std::int64_t>(out.state) != row_state
        || out.created_at_ms != row_created) {
        throw std::runtime_error(
            "dialogue responder intent index/document mismatch");
    }
    return out;
}

LocalGooseDialogueResponderDisposition read_disposition_row(
    sqlite3_stmt* statement)
{
    const auto row_consumer = text(statement, 0);
    const auto row_sequence = sqlite3_column_int64(statement, 1);
    const auto row_event = text(statement, 2);
    const auto row_kind = sqlite3_column_int64(statement, 3);
    const auto row_state = sqlite3_column_int64(statement, 4);
    const auto row_intent = optional_text(statement, 5);
    const auto row_created = sqlite3_column_int64(statement, 6);
    if (row_sequence <= 0 || row_kind < 0 || row_state < 0 || row_created < 0) {
        throw std::runtime_error(
            "invalid dialogue responder disposition index columns");
    }
    auto out = parse_disposition(text(statement, 7));
    if (out.consumer_id != row_consumer
        || out.completion_sequence != static_cast<std::uint64_t>(row_sequence)
        || out.event_id != row_event
        || static_cast<std::int64_t>(out.kind) != row_kind
        || static_cast<std::int64_t>(out.state) != row_state
        || out.intent_id != row_intent
        || out.created_at_ms != row_created) {
        throw std::runtime_error(
            "dialogue responder disposition index/document mismatch");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderLease> find_lease(
    sqlite3* database, const std::string& lease_id)
{
    Statement statement(
        database,
        "SELECT thread_alias,state,turns_committed,document "
        "FROM local_goose_dialogue_responder_lease WHERE lease_id=?1");
    bind_text(database, statement.get(), 1, lease_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_lease_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate dialogue responder lease row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderLease> find_active_lease(
    sqlite3* database, const std::string& thread_alias)
{
    Statement statement(
        database,
        "SELECT thread_alias,state,turns_committed,document "
        "FROM local_goose_dialogue_responder_lease "
        "WHERE thread_alias=?1 AND state=0");
    bind_text(database, statement.get(), 1, thread_alias);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_lease_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate active dialogue responder lease");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderIntent> find_intent(
    sqlite3* database, const std::string& intent_id)
{
    Statement statement(
        database,
        "SELECT lease_id,event_id,request_id,state,created_at_ms,document "
        "FROM local_goose_dialogue_responder_intent WHERE intent_id=?1");
    bind_text(database, statement.get(), 1, intent_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_intent_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate dialogue responder intent row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderIntent> find_trigger_intent(
    sqlite3* database, const std::string& lease_id, const std::string& event_id)
{
    Statement statement(
        database,
        "SELECT lease_id,event_id,request_id,state,created_at_ms,document "
        "FROM local_goose_dialogue_responder_intent "
        "WHERE lease_id=?1 AND event_id=?2");
    bind_text(database, statement.get(), 1, lease_id);
    bind_text(database, statement.get(), 2, event_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_intent_row(statement.get());
}

std::optional<LocalGooseDialogueResponderDisposition> find_disposition(
    sqlite3* database, const std::string& disposition_id)
{
    Statement statement(
        database,
        "SELECT consumer_id,completion_sequence,event_id,kind,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_disposition "
        "WHERE disposition_id=?1");
    bind_text(database, statement.get(), 1, disposition_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_disposition_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate dialogue responder disposition row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderDisposition> find_disposition_for_event(
    sqlite3* database,
    const std::string& consumer_id,
    const std::string& event_id)
{
    Statement statement(
        database,
        "SELECT consumer_id,completion_sequence,event_id,kind,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_disposition "
        "WHERE consumer_id=?1 AND event_id=?2");
    bind_text(database, statement.get(), 1, consumer_id);
    bind_text(database, statement.get(), 2, event_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_disposition_row(statement.get());
}

std::optional<LocalGooseDialogueResponderDisposition> find_disposition_for_sequence(
    sqlite3* database,
    const std::string& consumer_id,
    const std::uint64_t sequence)
{
    Statement statement(
        database,
        "SELECT consumer_id,completion_sequence,event_id,kind,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_disposition "
        "WHERE consumer_id=?1 AND completion_sequence=?2");
    bind_text(database, statement.get(), 1, consumer_id);
    bind_uint64(database, statement.get(), 2, sequence);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_disposition_row(statement.get());
}

bool same_lease(const LocalGooseDialogueResponderLease& a,
                const LocalGooseDialogueResponderLease& b)
{
    return lease_json(a) == lease_json(b);
}

bool same_disposition(const LocalGooseDialogueResponderDisposition& a,
                      const LocalGooseDialogueResponderDisposition& b)
{
    return disposition_json(a) == disposition_json(b);
}

bool checked_next_eligible(const std::int64_t base,
                           const std::int64_t interval,
                           std::int64_t& out) noexcept
{
    if (base < 0 || interval < 0
        || base > std::numeric_limits<std::int64_t>::max() - interval) {
        return false;
    }
    out = base + interval;
    return true;
}

void update_lease(sqlite3* database,
                  const LocalGooseDialogueResponderLease& lease,
                  const LocalGooseDialogueResponderLeaseState expected_state,
                  const std::uint64_t expected_turns)
{
    Statement statement(
        database,
        "UPDATE local_goose_dialogue_responder_lease "
        "SET state=?2,turns_committed=?3,document=?4 "
        "WHERE lease_id=?1 AND state=?5 AND turns_committed=?6");
    bind_text(database, statement.get(), 1, lease.lease_id);
    bind_int64(database, statement.get(), 2, static_cast<std::int64_t>(lease.state));
    bind_uint64(database, statement.get(), 3, lease.turns_committed);
    bind_text(database, statement.get(), 4, lease_json(lease).dump());
    bind_int64(database, statement.get(), 5, static_cast<std::int64_t>(expected_state));
    bind_uint64(database, statement.get(), 6, expected_turns);
    if (sqlite3_step(statement.get()) != SQLITE_DONE
        || sqlite3_changes(database) != 1) {
        throw std::runtime_error("dialogue responder lease update lost CAS");
    }
}

void update_intent(sqlite3* database,
                   const LocalGooseDialogueResponderIntent& intent,
                   const LocalGooseDialogueResponderIntentState expected_state)
{
    Statement statement(
        database,
        "UPDATE local_goose_dialogue_responder_intent "
        "SET state=?2,document=?3 WHERE intent_id=?1 AND state=?4");
    bind_text(database, statement.get(), 1, intent.intent_id);
    bind_int64(database, statement.get(), 2, static_cast<std::int64_t>(intent.state));
    bind_text(database, statement.get(), 3, intent_json(intent).dump());
    bind_int64(database, statement.get(), 4,
               static_cast<std::int64_t>(expected_state));
    if (sqlite3_step(statement.get()) != SQLITE_DONE
        || sqlite3_changes(database) != 1) {
        throw std::runtime_error("dialogue responder intent update lost CAS");
    }
}

void update_disposition(
    sqlite3* database,
    const LocalGooseDialogueResponderDisposition& disposition,
    const LocalGooseDialogueResponderDispositionState expected_state)
{
    Statement statement(
        database,
        "UPDATE local_goose_dialogue_responder_disposition "
        "SET state=?2,intent_id=?3,document=?4 "
        "WHERE disposition_id=?1 AND state=?5");
    bind_text(database, statement.get(), 1, disposition.disposition_id);
    bind_int64(database, statement.get(), 2,
               static_cast<std::int64_t>(disposition.state));
    if (disposition.intent_id) {
        bind_text(database, statement.get(), 3, *disposition.intent_id);
    } else {
        bind_null(database, statement.get(), 3);
    }
    bind_text(database, statement.get(), 4, disposition_json(disposition).dump());
    bind_int64(database, statement.get(), 5,
               static_cast<std::int64_t>(expected_state));
    if (sqlite3_step(statement.get()) != SQLITE_DONE
        || sqlite3_changes(database) != 1) {
        throw std::runtime_error("dialogue responder disposition update lost CAS");
    }
}

} // namespace

LocalGooseDialogueResponderStore::LocalGooseDialogueResponderStore(
    const std::string& path,
    const LocalGooseDialogueResponderStoreOpenMode open_mode)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_) : "cannot open dialogue responder SQLite";
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
        if (schema > local_goose_dialogue_responder_sidecar_schema) {
            throw std::runtime_error("unsupported dialogue responder schema");
        }
        if (schema == 0) {
            if (!user_table_names(database_).empty()) {
                throw std::runtime_error(
                    "unversioned dialogue responder sidecar is not empty");
            }
            create_lease_table(database_);
            create_intent_table(database_);
            create_disposition_table(database_);
            execute(database_, "PRAGMA user_version=2;");
        } else if (schema == 1) {
            if (!exact_table_set(
                    database_,
                    {"local_goose_dialogue_responder_lease",
                     "local_goose_dialogue_responder_intent"})) {
                throw std::runtime_error("dialogue responder v1 table set differs");
            }
            if (open_mode
                == LocalGooseDialogueResponderStoreOpenMode::allow_schema_upgrade) {
                create_disposition_table(database_);
                execute(database_, "PRAGMA user_version=2;");
            }
        } else if (schema == 2) {
            if (!exact_table_set(
                    database_,
                    {"local_goose_dialogue_responder_lease",
                     "local_goose_dialogue_responder_intent",
                     "local_goose_dialogue_responder_disposition"})) {
                throw std::runtime_error("dialogue responder v2 table set differs");
            }
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderStore::~LocalGooseDialogueResponderStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseDialogueResponderLease>
LocalGooseDialogueResponderStore::find_lease(const std::string& lease_id) const
{
    if (!safe_identifier(lease_id)) return std::nullopt;
    return ::gaudere_agent::find_lease(database_, lease_id);
}

std::optional<LocalGooseDialogueResponderLease>
LocalGooseDialogueResponderStore::find_active_lease(
    const std::string& thread_alias) const
{
    if (!safe_identifier(thread_alias)) return std::nullopt;
    return ::gaudere_agent::find_active_lease(database_, thread_alias);
}

LocalGooseDialogueResponderLeaseWrite
LocalGooseDialogueResponderStore::create_lease(
    const LocalGooseDialogueResponderLease& lease)
{
    LocalGooseDialogueResponderLeaseWrite out;
    using State = LocalGooseDialogueResponderLeaseState;
    if (!valid_local_goose_dialogue_responder_lease(lease)
        || lease.state != State::active
        || lease.turns_committed != 0
        || lease.next_eligible_at_ms != lease.issued_at_ms) {
        out.detail = "invalid initial dialogue responder lease";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = ::gaudere_agent::find_lease(database_, lease.lease_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.lease = existing;
            out.result = same_lease(*existing, lease)
                ? LocalGooseDialogueResponderStoreResult::duplicate
                : LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = out.result == LocalGooseDialogueResponderStoreResult::duplicate
                ? "dialogue responder lease already exists"
                : "dialogue responder lease id conflicts";
            return out;
        }
        const auto active =
            ::gaudere_agent::find_active_lease(database_, lease.thread_alias);
        if (active) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.lease = active;
            out.detail = "preferred thread already has an active responder lease";
            return out;
        }
        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_responder_lease("
            "lease_id,thread_alias,state,turns_committed,document)"
            "VALUES(?1,?2,0,0,?3)");
        bind_text(database_, statement.get(), 1, lease.lease_id);
        bind_text(database_, statement.get(), 2, lease.thread_alias);
        bind_text(database_, statement.get(), 3, lease_json(lease).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.lease = lease;
        out.detail = "dialogue responder lease created";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderLeaseWrite
LocalGooseDialogueResponderStore::close_lease(
    const std::string& lease_id,
    const LocalGooseDialogueResponderLeaseState terminal_state,
    const std::string& reason,
    const std::int64_t terminal_at_ms)
{
    LocalGooseDialogueResponderLeaseWrite out;
    using State = LocalGooseDialogueResponderLeaseState;
    if (!safe_identifier(lease_id)
        || !(terminal_state == State::expired || terminal_state == State::revoked
             || terminal_state == State::manual_review)
        || reason.empty() || reason.size() > max_reason_bytes
        || terminal_at_ms < 0) {
        out.detail = "invalid responder lease terminal transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto lease = ::gaudere_agent::find_lease(database_, lease_id);
        if (!lease) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder lease not found";
            return out;
        }
        if (lease->state == terminal_state
            && lease->terminal_reason == reason
            && lease->terminal_at_ms
                == std::optional<std::int64_t>{terminal_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.lease = lease;
            out.detail = "dialogue responder lease already closed";
            return out;
        }
        if (lease->state != State::active
            || terminal_at_ms < lease->issued_at_ms
            || (terminal_state == State::expired
                && terminal_at_ms < lease->expires_at_ms)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.lease = lease;
            out.detail = "dialogue responder lease close conflicts";
            return out;
        }
        const auto expected_state = lease->state;
        const auto expected_turns = lease->turns_committed;
        lease->state = terminal_state;
        lease->terminal_reason = reason;
        lease->terminal_at_ms = terminal_at_ms;
        if (!valid_local_goose_dialogue_responder_lease(*lease)) {
            throw std::runtime_error("terminal responder lease became invalid");
        }
        update_lease(database_, *lease, expected_state, expected_turns);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.lease = lease;
        out.detail = "dialogue responder lease closed";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

std::optional<LocalGooseDialogueResponderIntent>
LocalGooseDialogueResponderStore::find_intent(const std::string& intent_id) const
{
    if (!prefixed_sha256(intent_id, intent_prefix)) return std::nullopt;
    return ::gaudere_agent::find_intent(database_, intent_id);
}

std::vector<LocalGooseDialogueResponderIntent>
LocalGooseDialogueResponderStore::intents_in_state(
    const LocalGooseDialogueResponderIntentState state,
    const std::size_t limit) const
{
    std::vector<LocalGooseDialogueResponderIntent> out;
    if (limit == 0 || limit > 1024) return out;
    Statement statement(
        database_,
        "SELECT lease_id,event_id,request_id,state,created_at_ms,document "
        "FROM local_goose_dialogue_responder_intent "
        "WHERE state=?1 ORDER BY created_at_ms,intent_id LIMIT ?2");
    bind_int64(database_, statement.get(), 1, static_cast<std::int64_t>(state));
    bind_int64(database_, statement.get(), 2, static_cast<std::int64_t>(limit));
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
        out.push_back(read_intent_row(statement.get()));
    }
    return out;
}

LocalGooseDialogueResponderIntentWrite
LocalGooseDialogueResponderStore::prepare_intent(
    const LocalGooseDialogueResponderIntent& intent)
{
    LocalGooseDialogueResponderIntentWrite out;
    if (!valid_local_goose_dialogue_responder_intent(intent)
        || intent.state != LocalGooseDialogueResponderIntentState::prepared) {
        out.detail = "invalid initial dialogue responder intent";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = ::gaudere_agent::find_intent(database_, intent.intent_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.intent = existing;
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.detail = "dialogue responder intent already prepared";
            return out;
        }
        const auto trigger = find_trigger_intent(
            database_, intent.lease_id, intent.event_id);
        if (trigger) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = trigger;
            out.detail = "lease already has an intent for this completion event";
            return out;
        }
        const auto lease = ::gaudere_agent::find_lease(database_, intent.lease_id);
        const auto bit =
            local_goose_dialogue_responder_message_kind_bit(intent.message_kind);
        if (!lease
            || lease->state != LocalGooseDialogueResponderLeaseState::active
            || lease->thread_alias != intent.thread_alias
            || lease->speaker_kind != intent.speaker_kind
            || lease->speaker_id != intent.speaker_id
            || (lease->allowed_message_kinds & bit) == 0
            || lease->turns_committed >= lease->max_system_turns
            || intent.created_at_ms < lease->issued_at_ms
            || intent.created_at_ms >= lease->expires_at_ms
            || intent.expires_at_ms > lease->expires_at_ms
            || intent.created_at_ms < lease->next_eligible_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "intent is not admitted by active responder lease";
            return out;
        }
        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_responder_intent("
            "intent_id,lease_id,event_id,request_id,state,created_at_ms,document)"
            "VALUES(?1,?2,?3,?4,0,?5,?6)");
        bind_text(database_, statement.get(), 1, intent.intent_id);
        bind_text(database_, statement.get(), 2, intent.lease_id);
        bind_text(database_, statement.get(), 3, intent.event_id);
        bind_text(database_, statement.get(), 4, intent.request_id);
        bind_int64(database_, statement.get(), 5, intent.created_at_ms);
        bind_text(database_, statement.get(), 6, intent_json(intent).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.intent = intent;
        out.detail = "dialogue responder intent prepared";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderIntentWrite
LocalGooseDialogueResponderStore::commit_submission(
    const std::string& intent_id,
    const std::string& task_id,
    const std::uint64_t resulting_thread_revision,
    const std::int64_t committed_at_ms)
{
    LocalGooseDialogueResponderIntentWrite out;
    if (!prefixed_sha256(intent_id, intent_prefix)
        || !prefixed_sha256(task_id, v3_prefix)
        || resulting_thread_revision > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        || committed_at_ms < 0) {
        out.detail = "invalid responder submission reconciliation";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto intent = ::gaudere_agent::find_intent(database_, intent_id);
        if (!intent) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder intent not found";
            return out;
        }
        if ((intent->state == LocalGooseDialogueResponderIntentState::submitted
             || intent->state == LocalGooseDialogueResponderIntentState::completed)
            && intent->submitted_task_id == task_id
            && intent->submitted_thread_revision
                == std::optional<std::uint64_t>{resulting_thread_revision}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.intent = intent;
            out.detail = "responder submission already reconciled";
            return out;
        }
        if (intent->state != LocalGooseDialogueResponderIntentState::prepared
            || resulting_thread_revision != intent->expected_thread_revision + 1
            || committed_at_ms < intent->created_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = intent;
            out.detail = "responder submission reconciliation conflicts";
            return out;
        }
        auto lease = ::gaudere_agent::find_lease(database_, intent->lease_id);
        if (!lease || lease->turns_committed >= lease->max_system_turns) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = intent;
            out.detail = "responder lease cannot account another turn";
            return out;
        }
        std::int64_t next_eligible = 0;
        if (!checked_next_eligible(
                committed_at_ms, lease->min_interval_ms, next_eligible)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = intent;
            out.detail = "responder next eligible time would overflow";
            return out;
        }
        const auto old_intent_state = intent->state;
        intent->state = LocalGooseDialogueResponderIntentState::submitted;
        intent->submitted_task_id = task_id;
        intent->submitted_thread_revision = resulting_thread_revision;
        intent->committed_at_ms = committed_at_ms;
        if (!valid_local_goose_dialogue_responder_intent(*intent)) {
            throw std::runtime_error("reconciled responder intent became invalid");
        }
        update_intent(database_, *intent, old_intent_state);
        const auto old_lease_state = lease->state;
        const auto old_turns = lease->turns_committed;
        ++lease->turns_committed;
        lease->next_eligible_at_ms = next_eligible;
        if (lease->state == LocalGooseDialogueResponderLeaseState::active
            && lease->turns_committed == lease->max_system_turns) {
            lease->state = LocalGooseDialogueResponderLeaseState::exhausted;
            lease->terminal_reason = "quota_exhausted";
            lease->terminal_at_ms = committed_at_ms;
        }
        if (!valid_local_goose_dialogue_responder_lease(*lease)) {
            throw std::runtime_error("accounted responder lease became invalid");
        }
        update_lease(database_, *lease, old_lease_state, old_turns);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.intent = intent;
        out.detail = "responder submission reconciled";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderIntentWrite
LocalGooseDialogueResponderStore::complete_intent(
    const std::string& intent_id,
    const std::int64_t completed_at_ms)
{
    LocalGooseDialogueResponderIntentWrite out;
    if (!prefixed_sha256(intent_id, intent_prefix) || completed_at_ms < 0) {
        out.detail = "invalid responder completion transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto intent = ::gaudere_agent::find_intent(database_, intent_id);
        if (!intent) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder intent not found";
            return out;
        }
        if (intent->state == LocalGooseDialogueResponderIntentState::completed
            && intent->completed_at_ms
                == std::optional<std::int64_t>{completed_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.intent = intent;
            out.detail = "dialogue responder intent already completed";
            return out;
        }
        if (intent->state != LocalGooseDialogueResponderIntentState::submitted
            || !intent->committed_at_ms
            || completed_at_ms < *intent->committed_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = intent;
            out.detail = "dialogue responder completion conflicts";
            return out;
        }
        const auto old_state = intent->state;
        intent->state = LocalGooseDialogueResponderIntentState::completed;
        intent->completed_at_ms = completed_at_ms;
        if (!valid_local_goose_dialogue_responder_intent(*intent)) {
            throw std::runtime_error("completed responder intent became invalid");
        }
        update_intent(database_, *intent, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.intent = intent;
        out.detail = "dialogue responder intent completed";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderIntentWrite
LocalGooseDialogueResponderStore::terminalize_intent(
    const std::string& intent_id,
    const LocalGooseDialogueResponderIntentState terminal_state,
    const std::string& reason)
{
    LocalGooseDialogueResponderIntentWrite out;
    using State = LocalGooseDialogueResponderIntentState;
    if (!prefixed_sha256(intent_id, intent_prefix)
        || !(terminal_state == State::conflict || terminal_state == State::expired
             || terminal_state == State::manual_review)
        || reason.empty() || reason.size() > max_reason_bytes) {
        out.detail = "invalid responder intent terminal transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto intent = ::gaudere_agent::find_intent(database_, intent_id);
        if (!intent) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder intent not found";
            return out;
        }
        if (intent->state == terminal_state && intent->terminal_reason == reason) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.intent = intent;
            out.detail = "dialogue responder terminal intent already recorded";
            return out;
        }
        const bool allowed = intent->state == State::prepared
            || (intent->state == State::submitted
                && terminal_state == State::manual_review);
        if (!allowed) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.intent = intent;
            out.detail = "dialogue responder terminal transition conflicts";
            return out;
        }
        const auto old_state = intent->state;
        intent->state = terminal_state;
        intent->terminal_reason = reason;
        if (!valid_local_goose_dialogue_responder_intent(*intent)) {
            throw std::runtime_error("terminal responder intent became invalid");
        }
        update_intent(database_, *intent, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.intent = intent;
        out.detail = "dialogue responder intent terminalized";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

std::optional<LocalGooseDialogueResponderDisposition>
LocalGooseDialogueResponderStore::find_disposition(
    const std::string& disposition_id) const
{
    if (!prefixed_sha256(disposition_id, disposition_prefix)) return std::nullopt;
    return ::gaudere_agent::find_disposition(database_, disposition_id);
}

std::optional<LocalGooseDialogueResponderDisposition>
LocalGooseDialogueResponderStore::find_disposition_for_event(
    const std::string& consumer_id,
    const std::string& event_id) const
{
    if (!safe_identifier(consumer_id)
        || !prefixed_sha256(event_id, completion_event_prefix)) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_disposition_for_event(
        database_, consumer_id, event_id);
}

std::vector<LocalGooseDialogueResponderDisposition>
LocalGooseDialogueResponderStore::dispositions_in_state(
    const LocalGooseDialogueResponderDispositionState state,
    const std::size_t limit) const
{
    std::vector<LocalGooseDialogueResponderDisposition> out;
    if (limit == 0 || limit > 1024) return out;
    Statement statement(
        database_,
        "SELECT consumer_id,completion_sequence,event_id,kind,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_disposition "
        "WHERE state=?1 ORDER BY created_at_ms,disposition_id LIMIT ?2");
    bind_int64(database_, statement.get(), 1, static_cast<std::int64_t>(state));
    bind_int64(database_, statement.get(), 2, static_cast<std::int64_t>(limit));
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
        out.push_back(read_disposition_row(statement.get()));
    }
    return out;
}

LocalGooseDialogueResponderDispositionWrite
LocalGooseDialogueResponderStore::prepare_disposition(
    const LocalGooseDialogueResponderDisposition& disposition)
{
    LocalGooseDialogueResponderDispositionWrite out;
    if (!valid_local_goose_dialogue_responder_disposition(disposition)
        || disposition.state
            != LocalGooseDialogueResponderDispositionState::prepared) {
        out.detail = "invalid initial dialogue responder disposition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing =
            ::gaudere_agent::find_disposition(database_, disposition.disposition_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.disposition = existing;
            out.result = same_disposition(*existing, disposition)
                ? LocalGooseDialogueResponderStoreResult::duplicate
                : LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = out.result == LocalGooseDialogueResponderStoreResult::duplicate
                ? "dialogue responder disposition already prepared"
                : "dialogue responder disposition id conflicts";
            return out;
        }
        const auto event = ::gaudere_agent::find_disposition_for_event(
            database_, disposition.consumer_id, disposition.event_id);
        if (event) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = event;
            out.detail = "responder consumer already has a disposition for event";
            return out;
        }
        const auto sequence = ::gaudere_agent::find_disposition_for_sequence(
            database_, disposition.consumer_id, disposition.completion_sequence);
        if (sequence) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = sequence;
            out.detail = "responder consumer already has a disposition for sequence";
            return out;
        }
        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_responder_disposition("
            "disposition_id,consumer_id,completion_sequence,event_id,kind,state,"
            "intent_id,created_at_ms,document)"
            "VALUES(?1,?2,?3,?4,?5,0,NULL,?6,?7)");
        bind_text(database_, statement.get(), 1, disposition.disposition_id);
        bind_text(database_, statement.get(), 2, disposition.consumer_id);
        bind_uint64(database_, statement.get(), 3, disposition.completion_sequence);
        bind_text(database_, statement.get(), 4, disposition.event_id);
        bind_int64(database_, statement.get(), 5,
                   static_cast<std::int64_t>(disposition.kind));
        bind_int64(database_, statement.get(), 6, disposition.created_at_ms);
        bind_text(database_, statement.get(), 7, disposition_json(disposition).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.disposition = disposition;
        out.detail = "dialogue responder disposition prepared";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDispositionWrite
LocalGooseDialogueResponderStore::bind_disposition_intent(
    const std::string& disposition_id,
    const std::string& intent_id,
    const std::int64_t bound_at_ms)
{
    LocalGooseDialogueResponderDispositionWrite out;
    using DState = LocalGooseDialogueResponderDispositionState;
    using DKind = LocalGooseDialogueResponderDispositionKind;
    if (!prefixed_sha256(disposition_id, disposition_prefix)
        || !prefixed_sha256(intent_id, intent_prefix)
        || bound_at_ms < 0) {
        out.detail = "invalid responder disposition intent binding";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto disposition = ::gaudere_agent::find_disposition(database_, disposition_id);
        if (!disposition) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder disposition not found";
            return out;
        }
        if (disposition->state == DState::intervention_bound
            && disposition->intent_id == std::optional<std::string>{intent_id}
            && disposition->bound_at_ms
                == std::optional<std::int64_t>{bound_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.disposition = disposition;
            out.detail = "dialogue responder disposition already bound";
            return out;
        }
        const auto intent = ::gaudere_agent::find_intent(database_, intent_id);
        if (disposition->kind != DKind::intervention
            || disposition->state != DState::prepared
            || bound_at_ms < disposition->created_at_ms
            || !intent
            || intent->state != LocalGooseDialogueResponderIntentState::prepared
            || bound_at_ms < intent->created_at_ms
            || intent->thread_alias != disposition->thread_alias
            || intent->completion_sequence != disposition->completion_sequence
            || intent->event_id != disposition->event_id
            || intent->result_sha256 != disposition->result_sha256
            || intent->triggering_task_id != disposition->task_id
            || intent->expected_thread_revision != disposition->thread_revision
            || intent->expected_head_task_id != disposition->task_id) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = disposition;
            out.detail = "responder disposition intent binding conflicts";
            return out;
        }
        const auto old_state = disposition->state;
        disposition->state = DState::intervention_bound;
        disposition->intent_id = intent_id;
        disposition->bound_at_ms = bound_at_ms;
        if (!valid_local_goose_dialogue_responder_disposition(*disposition)) {
            throw std::runtime_error("bound responder disposition became invalid");
        }
        update_disposition(database_, *disposition, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.disposition = disposition;
        out.detail = "dialogue responder disposition bound to intent";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDispositionWrite
LocalGooseDialogueResponderStore::acknowledge_disposition(
    const std::string& disposition_id,
    const std::int64_t reconciled_at_ms)
{
    LocalGooseDialogueResponderDispositionWrite out;
    using DState = LocalGooseDialogueResponderDispositionState;
    using DKind = LocalGooseDialogueResponderDispositionKind;
    if (!prefixed_sha256(disposition_id, disposition_prefix)
        || reconciled_at_ms < 0) {
        out.detail = "invalid responder disposition ACK reconciliation";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto disposition = ::gaudere_agent::find_disposition(database_, disposition_id);
        if (!disposition) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder disposition not found";
            return out;
        }
        if (disposition->state == DState::acknowledged
            && disposition->reconciled_at_ms
                == std::optional<std::int64_t>{reconciled_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.disposition = disposition;
            out.detail = "dialogue responder disposition already acknowledged";
            return out;
        }
        if (disposition->state != DState::prepared
            || disposition->kind == DKind::intervention
            || reconciled_at_ms < disposition->created_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = disposition;
            out.detail = "responder disposition ACK reconciliation conflicts";
            return out;
        }
        const auto old_state = disposition->state;
        disposition->state = DState::acknowledged;
        disposition->reconciled_at_ms = reconciled_at_ms;
        if (!valid_local_goose_dialogue_responder_disposition(*disposition)) {
            throw std::runtime_error(
                "acknowledged responder disposition became invalid");
        }
        update_disposition(database_, *disposition, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.disposition = disposition;
        out.detail = "dialogue responder disposition ACK reconciled";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDispositionWrite
LocalGooseDialogueResponderStore::complete_disposition(
    const std::string& disposition_id,
    const std::int64_t reconciled_at_ms)
{
    LocalGooseDialogueResponderDispositionWrite out;
    using DState = LocalGooseDialogueResponderDispositionState;
    using DKind = LocalGooseDialogueResponderDispositionKind;
    if (!prefixed_sha256(disposition_id, disposition_prefix)
        || reconciled_at_ms < 0) {
        out.detail = "invalid responder intervention disposition reconciliation";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto disposition = ::gaudere_agent::find_disposition(database_, disposition_id);
        if (!disposition) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder disposition not found";
            return out;
        }
        if (disposition->state == DState::completed
            && disposition->reconciled_at_ms
                == std::optional<std::int64_t>{reconciled_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.disposition = disposition;
            out.detail = "dialogue responder disposition already completed";
            return out;
        }
        if (disposition->kind != DKind::intervention
            || disposition->state != DState::intervention_bound
            || !disposition->intent_id || !disposition->bound_at_ms
            || reconciled_at_ms < *disposition->bound_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = disposition;
            out.detail = "responder intervention disposition conflicts";
            return out;
        }
        const auto intent = ::gaudere_agent::find_intent(
            database_, *disposition->intent_id);
        if (!intent
            || intent->state != LocalGooseDialogueResponderIntentState::completed
            || !intent->completed_at_ms
            || reconciled_at_ms < *intent->completed_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = disposition;
            out.detail = "bound responder intent is not durably completed";
            return out;
        }
        const auto old_state = disposition->state;
        disposition->state = DState::completed;
        disposition->reconciled_at_ms = reconciled_at_ms;
        if (!valid_local_goose_dialogue_responder_disposition(*disposition)) {
            throw std::runtime_error("completed responder disposition became invalid");
        }
        update_disposition(database_, *disposition, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.disposition = disposition;
        out.detail = "dialogue responder disposition completed";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDispositionWrite
LocalGooseDialogueResponderStore::terminalize_disposition(
    const std::string& disposition_id,
    const std::string& reason,
    const std::int64_t terminal_at_ms)
{
    LocalGooseDialogueResponderDispositionWrite out;
    using DState = LocalGooseDialogueResponderDispositionState;
    if (!prefixed_sha256(disposition_id, disposition_prefix)
        || reason.empty() || reason.size() > max_reason_bytes
        || terminal_at_ms < 0) {
        out.detail = "invalid responder disposition terminal transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto disposition = ::gaudere_agent::find_disposition(database_, disposition_id);
        if (!disposition) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "dialogue responder disposition not found";
            return out;
        }
        if (disposition->state == DState::manual_review
            && disposition->terminal_reason == reason
            && disposition->terminal_at_ms
                == std::optional<std::int64_t>{terminal_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.disposition = disposition;
            out.detail = "dialogue responder disposition already in manual review";
            return out;
        }
        if (!(disposition->state == DState::prepared
              || disposition->state == DState::intervention_bound)
            || terminal_at_ms < disposition->created_at_ms
            || (disposition->bound_at_ms
                && terminal_at_ms < *disposition->bound_at_ms)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.disposition = disposition;
            out.detail = "dialogue responder disposition terminal transition conflicts";
            return out;
        }
        const auto old_state = disposition->state;
        disposition->state = DState::manual_review;
        disposition->terminal_reason = reason;
        disposition->terminal_at_ms = terminal_at_ms;
        if (!valid_local_goose_dialogue_responder_disposition(*disposition)) {
            throw std::runtime_error("terminal responder disposition became invalid");
        }
        update_disposition(database_, *disposition, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.disposition = disposition;
        out.detail = "dialogue responder disposition requires manual review";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

} // namespace gaudere_agent
