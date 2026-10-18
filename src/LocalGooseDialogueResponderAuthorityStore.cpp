#include "LocalGooseDialogueResponderAuthorityStore.hpp"

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
constexpr const char* grant_prefix =
    "cognition.local-goose-dialogue.responder-grant.v1:";
constexpr const char* decision_prefix =
    "cognition.local-goose-dialogue.responder-decision.v1:";
constexpr const char* intent_prefix =
    "cognition.local-goose-dialogue.responder-intent.v1:";

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
            "dialogue responder authority sidecar must be a current-user regular file mode 0600");
    }
}

void ensure_existing_secure_file(const std::string& path)
{
    const int descriptor = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw std::runtime_error(
            "dialogue responder authority requires an existing responder sidecar");
    }
    close(descriptor);
    verify_owner_mode(path);
}

void create_grant_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_grant("
        "grant_id TEXT PRIMARY KEY NOT NULL,"
        "request_id TEXT NOT NULL UNIQUE,"
        "thread_alias TEXT NOT NULL,"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),"
        "lease_id TEXT UNIQUE,"
        "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
        "document TEXT NOT NULL,"
        "FOREIGN KEY(lease_id) REFERENCES "
        "local_goose_dialogue_responder_lease(lease_id));");
}

void create_decision_table(sqlite3* database)
{
    execute(database,
        "CREATE TABLE local_goose_dialogue_responder_decision("
        "decision_id TEXT PRIMARY KEY NOT NULL,"
        "request_id TEXT NOT NULL UNIQUE,"
        "grant_id TEXT NOT NULL,"
        "lease_id TEXT NOT NULL,"
        "consumer_id TEXT NOT NULL,"
        "event_id TEXT NOT NULL,"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),"
        "intent_id TEXT UNIQUE,"
        "created_at_ms INTEGER NOT NULL CHECK(created_at_ms >= 0),"
        "document TEXT NOT NULL,"
        "UNIQUE(consumer_id,event_id),"
        "FOREIGN KEY(grant_id) REFERENCES "
        "local_goose_dialogue_responder_grant(grant_id),"
        "FOREIGN KEY(lease_id) REFERENCES "
        "local_goose_dialogue_responder_lease(lease_id),"
        "FOREIGN KEY(intent_id) REFERENCES "
        "local_goose_dialogue_responder_intent(intent_id));");
}

Json grant_json(const LocalGooseDialogueResponderGrant& grant)
{
    Json out{
        {"grant_id", grant.grant_id},
        {"request_id", grant.request_id},
        {"authority_source", grant.authority_source},
        {"thread_alias", grant.thread_alias},
        {"speaker_kind", grant.speaker_kind},
        {"speaker_id", grant.speaker_id},
        {"allowed_message_kinds", grant.allowed_message_kinds},
        {"purpose", grant.purpose},
        {"max_system_turns", grant.max_system_turns},
        {"max_lease_ttl_ms", grant.max_lease_ttl_ms},
        {"min_interval_ms", grant.min_interval_ms},
        {"created_at_ms", grant.created_at_ms},
        {"expires_at_ms", grant.expires_at_ms},
        {"state", static_cast<unsigned>(grant.state)},
        {"terminal_reason", grant.terminal_reason}
    };
    out["activated_lease_id"] = grant.activated_lease_id
        ? Json(*grant.activated_lease_id) : Json(nullptr);
    out["activated_at_ms"] = grant.activated_at_ms
        ? Json(*grant.activated_at_ms) : Json(nullptr);
    out["terminal_at_ms"] = grant.terminal_at_ms
        ? Json(*grant.terminal_at_ms) : Json(nullptr);
    return out;
}

Json decision_json(const LocalGooseDialogueResponderDecision& decision)
{
    Json out{
        {"decision_id", decision.decision_id},
        {"request_id", decision.request_id},
        {"authority_source", decision.authority_source},
        {"grant_id", decision.grant_id},
        {"lease_id", decision.lease_id},
        {"consumer_id", decision.consumer_id},
        {"completion_sequence", decision.completion_sequence},
        {"event_id", decision.event_id},
        {"result_sha256", decision.result_sha256},
        {"thread_alias", decision.thread_alias},
        {"thread_revision", decision.thread_revision},
        {"task_id", decision.task_id},
        {"observed_speaker_kind", decision.observed_speaker_kind},
        {"observed_speaker_id", decision.observed_speaker_id},
        {"expected_head_revision", decision.expected_head_revision},
        {"expected_head_task_id", decision.expected_head_task_id},
        {"responder_speaker_kind", decision.responder_speaker_kind},
        {"responder_speaker_id", decision.responder_speaker_id},
        {"message_kind", decision.message_kind},
        {"message", decision.message},
        {"intent_ttl_ms", decision.intent_ttl_ms},
        {"created_at_ms", decision.created_at_ms},
        {"state", static_cast<unsigned>(decision.state)},
        {"terminal_reason", decision.terminal_reason}
    };
    out["intent_id"] = decision.intent_id
        ? Json(*decision.intent_id) : Json(nullptr);
    out["bound_at_ms"] = decision.bound_at_ms
        ? Json(*decision.bound_at_ms) : Json(nullptr);
    out["completed_at_ms"] = decision.completed_at_ms
        ? Json(*decision.completed_at_ms) : Json(nullptr);
    out["terminal_at_ms"] = decision.terminal_at_ms
        ? Json(*decision.terminal_at_ms) : Json(nullptr);
    return out;
}

LocalGooseDialogueResponderGrant parse_grant(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderGrant out;
    out.grant_id = j.at("grant_id").get<std::string>();
    out.request_id = j.at("request_id").get<std::string>();
    out.authority_source = j.at("authority_source").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.speaker_kind = j.at("speaker_kind").get<std::string>();
    out.speaker_id = j.at("speaker_id").get<std::string>();
    out.allowed_message_kinds = j.at("allowed_message_kinds").get<std::uint32_t>();
    out.purpose = j.at("purpose").get<std::string>();
    out.max_system_turns = j.at("max_system_turns").get<std::uint64_t>();
    out.max_lease_ttl_ms = j.at("max_lease_ttl_ms").get<std::int64_t>();
    out.min_interval_ms = j.at("min_interval_ms").get<std::int64_t>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    out.expires_at_ms = j.at("expires_at_ms").get<std::int64_t>();
    out.state = static_cast<LocalGooseDialogueResponderGrantState>(
        j.at("state").get<unsigned>());
    if (!j.at("activated_lease_id").is_null()) {
        out.activated_lease_id = j.at("activated_lease_id").get<std::string>();
    }
    if (!j.at("activated_at_ms").is_null()) {
        out.activated_at_ms = j.at("activated_at_ms").get<std::int64_t>();
    }
    out.terminal_reason = j.at("terminal_reason").get<std::string>();
    if (!j.at("terminal_at_ms").is_null()) {
        out.terminal_at_ms = j.at("terminal_at_ms").get<std::int64_t>();
    }
    if (!valid_local_goose_dialogue_responder_grant(out)) {
        throw std::runtime_error("non-canonical dialogue responder grant row");
    }
    return out;
}

LocalGooseDialogueResponderDecision parse_decision(const std::string& raw)
{
    const auto j = Json::parse(raw);
    LocalGooseDialogueResponderDecision out;
    out.decision_id = j.at("decision_id").get<std::string>();
    out.request_id = j.at("request_id").get<std::string>();
    out.authority_source = j.at("authority_source").get<std::string>();
    out.grant_id = j.at("grant_id").get<std::string>();
    out.lease_id = j.at("lease_id").get<std::string>();
    out.consumer_id = j.at("consumer_id").get<std::string>();
    out.completion_sequence = j.at("completion_sequence").get<std::uint64_t>();
    out.event_id = j.at("event_id").get<std::string>();
    out.result_sha256 = j.at("result_sha256").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.thread_revision = j.at("thread_revision").get<std::uint64_t>();
    out.task_id = j.at("task_id").get<std::string>();
    out.observed_speaker_kind = j.at("observed_speaker_kind").get<std::string>();
    out.observed_speaker_id = j.at("observed_speaker_id").get<std::string>();
    out.expected_head_revision = j.at("expected_head_revision").get<std::uint64_t>();
    out.expected_head_task_id = j.at("expected_head_task_id").get<std::string>();
    out.responder_speaker_kind = j.at("responder_speaker_kind").get<std::string>();
    out.responder_speaker_id = j.at("responder_speaker_id").get<std::string>();
    out.message_kind = j.at("message_kind").get<std::string>();
    out.message = j.at("message").get<std::string>();
    out.intent_ttl_ms = j.at("intent_ttl_ms").get<std::int64_t>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    out.state = static_cast<LocalGooseDialogueResponderDecisionState>(
        j.at("state").get<unsigned>());
    if (!j.at("intent_id").is_null()) {
        out.intent_id = j.at("intent_id").get<std::string>();
    }
    if (!j.at("bound_at_ms").is_null()) {
        out.bound_at_ms = j.at("bound_at_ms").get<std::int64_t>();
    }
    if (!j.at("completed_at_ms").is_null()) {
        out.completed_at_ms = j.at("completed_at_ms").get<std::int64_t>();
    }
    out.terminal_reason = j.at("terminal_reason").get<std::string>();
    if (!j.at("terminal_at_ms").is_null()) {
        out.terminal_at_ms = j.at("terminal_at_ms").get<std::int64_t>();
    }
    if (!valid_local_goose_dialogue_responder_decision(out)) {
        throw std::runtime_error("non-canonical dialogue responder decision row");
    }
    return out;
}

LocalGooseDialogueResponderGrant read_grant_row(sqlite3_stmt* statement)
{
    const auto row_request = text(statement, 0);
    const auto row_thread = text(statement, 1);
    const auto row_state = sqlite3_column_int64(statement, 2);
    const auto row_lease = optional_text(statement, 3);
    const auto row_created = sqlite3_column_int64(statement, 4);
    if (row_state < 0 || row_created < 0) {
        throw std::runtime_error("invalid responder grant index columns");
    }
    auto out = parse_grant(text(statement, 5));
    if (out.request_id != row_request || out.thread_alias != row_thread
        || static_cast<std::int64_t>(out.state) != row_state
        || out.activated_lease_id != row_lease
        || out.created_at_ms != row_created) {
        throw std::runtime_error("responder grant index/document mismatch");
    }
    return out;
}

LocalGooseDialogueResponderDecision read_decision_row(sqlite3_stmt* statement)
{
    const auto row_request = text(statement, 0);
    const auto row_grant = text(statement, 1);
    const auto row_lease = text(statement, 2);
    const auto row_consumer = text(statement, 3);
    const auto row_event = text(statement, 4);
    const auto row_state = sqlite3_column_int64(statement, 5);
    const auto row_intent = optional_text(statement, 6);
    const auto row_created = sqlite3_column_int64(statement, 7);
    if (row_state < 0 || row_created < 0) {
        throw std::runtime_error("invalid responder decision index columns");
    }
    auto out = parse_decision(text(statement, 8));
    if (out.request_id != row_request || out.grant_id != row_grant
        || out.lease_id != row_lease || out.consumer_id != row_consumer
        || out.event_id != row_event
        || static_cast<std::int64_t>(out.state) != row_state
        || out.intent_id != row_intent
        || out.created_at_ms != row_created) {
        throw std::runtime_error("responder decision index/document mismatch");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderGrant> find_grant(
    sqlite3* database, const std::string& grant_id)
{
    Statement statement(database,
        "SELECT request_id,thread_alias,state,lease_id,created_at_ms,document "
        "FROM local_goose_dialogue_responder_grant WHERE grant_id=?1");
    bind_text(database, statement.get(), 1, grant_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_grant_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate responder grant row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderGrant> find_grant_by_request(
    sqlite3* database, const std::string& request_id)
{
    Statement statement(database,
        "SELECT request_id,thread_alias,state,lease_id,created_at_ms,document "
        "FROM local_goose_dialogue_responder_grant WHERE request_id=?1");
    bind_text(database, statement.get(), 1, request_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_grant_row(statement.get());
}

std::optional<LocalGooseDialogueResponderDecision> find_decision(
    sqlite3* database, const std::string& decision_id)
{
    Statement statement(database,
        "SELECT request_id,grant_id,lease_id,consumer_id,event_id,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_decision "
        "WHERE decision_id=?1");
    bind_text(database, statement.get(), 1, decision_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto out = read_decision_row(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error("duplicate responder decision row");
    }
    return out;
}

std::optional<LocalGooseDialogueResponderDecision> find_decision_by_request(
    sqlite3* database, const std::string& request_id)
{
    Statement statement(database,
        "SELECT request_id,grant_id,lease_id,consumer_id,event_id,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_decision "
        "WHERE request_id=?1");
    bind_text(database, statement.get(), 1, request_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_decision_row(statement.get());
}

std::optional<LocalGooseDialogueResponderDecision> find_decision_for_event(
    sqlite3* database,
    const std::string& consumer_id,
    const std::string& event_id)
{
    Statement statement(database,
        "SELECT request_id,grant_id,lease_id,consumer_id,event_id,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_decision "
        "WHERE consumer_id=?1 AND event_id=?2");
    bind_text(database, statement.get(), 1, consumer_id);
    bind_text(database, statement.get(), 2, event_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_decision_row(statement.get());
}

struct LeaseEvidence {
    std::string lease_id;
    std::string thread_alias;
    std::string speaker_kind;
    std::string speaker_id;
    std::uint32_t allowed_message_kinds = 0;
    std::string purpose;
    std::uint64_t max_system_turns = 0;
    std::uint64_t turns_committed = 0;
    std::int64_t issued_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::int64_t min_interval_ms = 0;
    std::int64_t next_eligible_at_ms = 0;
    std::int64_t state = -1;
};

std::optional<LeaseEvidence> find_lease_evidence(
    sqlite3* database, const std::string& lease_id)
{
    Statement statement(database,
        "SELECT document FROM local_goose_dialogue_responder_lease "
        "WHERE lease_id=?1");
    bind_text(database, statement.get(), 1, lease_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    const auto j = Json::parse(text(statement.get(), 0));
    LeaseEvidence out;
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
    out.state = static_cast<std::int64_t>(j.at("state").get<unsigned>());
    if (out.lease_id != lease_id) {
        throw std::runtime_error("responder lease evidence id mismatch");
    }
    return out;
}

struct IntentEvidence {
    std::string intent_id;
    std::string lease_id;
    std::string thread_alias;
    std::uint64_t completion_sequence = 0;
    std::string event_id;
    std::string result_sha256;
    std::string triggering_task_id;
    std::uint64_t expected_thread_revision = 0;
    std::string expected_head_task_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string message;
    std::int64_t created_at_ms = 0;
    std::int64_t expires_at_ms = 0;
    std::int64_t state = -1;
};

std::optional<IntentEvidence> find_intent_evidence(
    sqlite3* database, const std::string& intent_id)
{
    Statement statement(database,
        "SELECT document FROM local_goose_dialogue_responder_intent "
        "WHERE intent_id=?1");
    bind_text(database, statement.get(), 1, intent_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    const auto j = Json::parse(text(statement.get(), 0));
    IntentEvidence out;
    out.intent_id = j.at("intent_id").get<std::string>();
    out.lease_id = j.at("lease_id").get<std::string>();
    out.thread_alias = j.at("thread_alias").get<std::string>();
    out.completion_sequence = j.at("completion_sequence").get<std::uint64_t>();
    out.event_id = j.at("event_id").get<std::string>();
    out.result_sha256 = j.at("result_sha256").get<std::string>();
    out.triggering_task_id = j.at("triggering_task_id").get<std::string>();
    out.expected_thread_revision = j.at("expected_thread_revision").get<std::uint64_t>();
    out.expected_head_task_id = j.at("expected_head_task_id").get<std::string>();
    out.speaker_kind = j.at("speaker_kind").get<std::string>();
    out.speaker_id = j.at("speaker_id").get<std::string>();
    out.message_kind = j.at("message_kind").get<std::string>();
    out.message = j.at("message").get<std::string>();
    out.created_at_ms = j.at("created_at_ms").get<std::int64_t>();
    out.expires_at_ms = j.at("expires_at_ms").get<std::int64_t>();
    out.state = static_cast<std::int64_t>(j.at("state").get<unsigned>());
    if (out.intent_id != intent_id) {
        throw std::runtime_error("responder intent evidence id mismatch");
    }
    return out;
}

bool lease_within_grant(
    const LeaseEvidence& lease,
    const LocalGooseDialogueResponderGrant& grant) noexcept
{
    return lease.thread_alias == grant.thread_alias
        && lease.speaker_kind == grant.speaker_kind
        && lease.speaker_id == grant.speaker_id
        && lease.allowed_message_kinds != 0
        && (lease.allowed_message_kinds & ~grant.allowed_message_kinds) == 0
        && lease.purpose == grant.purpose
        && lease.max_system_turns > 0
        && lease.max_system_turns <= grant.max_system_turns
        && lease.turns_committed <= lease.max_system_turns
        && lease.issued_at_ms >= grant.created_at_ms
        && lease.expires_at_ms > lease.issued_at_ms
        && lease.expires_at_ms <= grant.expires_at_ms
        && lease.expires_at_ms - lease.issued_at_ms <= grant.max_lease_ttl_ms
        && lease.min_interval_ms >= grant.min_interval_ms;
}

bool intent_matches_decision(
    const IntentEvidence& intent,
    const LocalGooseDialogueResponderDecision& decision,
    const std::int64_t bound_at_ms) noexcept
{
    return intent.lease_id == decision.lease_id
        && intent.thread_alias == decision.thread_alias
        && intent.completion_sequence == decision.completion_sequence
        && intent.event_id == decision.event_id
        && intent.result_sha256 == decision.result_sha256
        && intent.triggering_task_id == decision.task_id
        && intent.expected_thread_revision == decision.expected_head_revision
        && intent.expected_head_task_id == decision.expected_head_task_id
        && intent.speaker_kind == decision.responder_speaker_kind
        && intent.speaker_id == decision.responder_speaker_id
        && intent.message_kind == decision.message_kind
        && intent.message == decision.message
        && intent.created_at_ms >= decision.created_at_ms
        && intent.created_at_ms <= bound_at_ms
        && intent.expires_at_ms > bound_at_ms
        && intent.expires_at_ms > intent.created_at_ms
        && intent.expires_at_ms - intent.created_at_ms <= decision.intent_ttl_ms
        && intent.state == 0;
}

bool completed_intervention_disposition(
    sqlite3* database,
    const LocalGooseDialogueResponderDecision& decision,
    const std::string& intent_id)
{
    Statement statement(database,
        "SELECT kind,state,intent_id FROM local_goose_dialogue_responder_disposition "
        "WHERE consumer_id=?1 AND event_id=?2");
    bind_text(database, statement.get(), 1, decision.consumer_id);
    bind_text(database, statement.get(), 2, decision.event_id);
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_ROW) return false;
    const auto kind = sqlite3_column_int64(statement.get(), 0);
    const auto state = sqlite3_column_int64(statement.get(), 1);
    const auto stored_intent = optional_text(statement.get(), 2);
    return kind == 3 && state == 3
        && stored_intent == std::optional<std::string>{intent_id};
}

void update_grant(
    sqlite3* database,
    const LocalGooseDialogueResponderGrant& grant,
    const LocalGooseDialogueResponderGrantState expected_state)
{
    Statement statement(database,
        "UPDATE local_goose_dialogue_responder_grant "
        "SET state=?2,lease_id=?3,document=?4 "
        "WHERE grant_id=?1 AND state=?5");
    bind_text(database, statement.get(), 1, grant.grant_id);
    bind_int64(database, statement.get(), 2, static_cast<std::int64_t>(grant.state));
    if (grant.activated_lease_id) {
        bind_text(database, statement.get(), 3, *grant.activated_lease_id);
    } else {
        bind_null(database, statement.get(), 3);
    }
    bind_text(database, statement.get(), 4, grant_json(grant).dump());
    bind_int64(database, statement.get(), 5, static_cast<std::int64_t>(expected_state));
    if (sqlite3_step(statement.get()) != SQLITE_DONE
        || sqlite3_changes(database) != 1) {
        throw std::runtime_error("responder grant update lost CAS");
    }
}

void update_decision(
    sqlite3* database,
    const LocalGooseDialogueResponderDecision& decision,
    const LocalGooseDialogueResponderDecisionState expected_state)
{
    Statement statement(database,
        "UPDATE local_goose_dialogue_responder_decision "
        "SET state=?2,intent_id=?3,document=?4 "
        "WHERE decision_id=?1 AND state=?5");
    bind_text(database, statement.get(), 1, decision.decision_id);
    bind_int64(database, statement.get(), 2,
               static_cast<std::int64_t>(decision.state));
    if (decision.intent_id) {
        bind_text(database, statement.get(), 3, *decision.intent_id);
    } else {
        bind_null(database, statement.get(), 3);
    }
    bind_text(database, statement.get(), 4, decision_json(decision).dump());
    bind_int64(database, statement.get(), 5,
               static_cast<std::int64_t>(expected_state));
    if (sqlite3_step(statement.get()) != SQLITE_DONE
        || sqlite3_changes(database) != 1) {
        throw std::runtime_error("responder decision update lost CAS");
    }
}

} // namespace

LocalGooseDialogueResponderAuthorityStore::
LocalGooseDialogueResponderAuthorityStore(
    const std::string& path,
    const LocalGooseDialogueResponderAuthorityStoreOpenMode open_mode)
{
    ensure_existing_secure_file(path);
    if (sqlite3_open_v2(path.c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_) : "cannot open responder authority SQLite";
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
        if (schema < 2
            || schema > local_goose_dialogue_responder_authority_sidecar_schema) {
            throw std::runtime_error("unsupported responder authority schema");
        }
        if (schema == 2) {
            if (!exact_table_set(database_,
                    {"local_goose_dialogue_responder_lease",
                     "local_goose_dialogue_responder_intent",
                     "local_goose_dialogue_responder_disposition"})) {
                throw std::runtime_error("responder authority v2 table set differs");
            }
            if (open_mode
                == LocalGooseDialogueResponderAuthorityStoreOpenMode::
                    allow_schema_upgrade) {
                create_grant_table(database_);
                create_decision_table(database_);
                execute(database_, "PRAGMA user_version=3;");
                schema_version_ = 3;
            } else {
                schema_version_ = 2;
            }
        } else {
            if (!exact_table_set(database_,
                    {"local_goose_dialogue_responder_lease",
                     "local_goose_dialogue_responder_intent",
                     "local_goose_dialogue_responder_disposition",
                     "local_goose_dialogue_responder_grant",
                     "local_goose_dialogue_responder_decision"})) {
                throw std::runtime_error("responder authority v3 table set differs");
            }
            schema_version_ = 3;
        }
        execute(database_, "COMMIT;");
    } catch (...) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseDialogueResponderAuthorityStore::~LocalGooseDialogueResponderAuthorityStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseDialogueResponderGrant>
LocalGooseDialogueResponderAuthorityStore::find_grant(
    const std::string& grant_id) const
{
    if (!authority_schema_ready() || !prefixed_sha256(grant_id, grant_prefix)) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_grant(database_, grant_id);
}

std::optional<LocalGooseDialogueResponderGrant>
LocalGooseDialogueResponderAuthorityStore::find_grant_by_request(
    const std::string& request_id) const
{
    if (!authority_schema_ready() || !safe_identifier(request_id)) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_grant_by_request(database_, request_id);
}

std::vector<LocalGooseDialogueResponderGrant>
LocalGooseDialogueResponderAuthorityStore::grants_in_state(
    const LocalGooseDialogueResponderGrantState state,
    const std::size_t limit) const
{
    std::vector<LocalGooseDialogueResponderGrant> out;
    if (!authority_schema_ready() || limit == 0 || limit > 1024) return out;
    Statement statement(database_,
        "SELECT request_id,thread_alias,state,lease_id,created_at_ms,document "
        "FROM local_goose_dialogue_responder_grant "
        "WHERE state=?1 ORDER BY created_at_ms,grant_id LIMIT ?2");
    bind_int64(database_, statement.get(), 1, static_cast<std::int64_t>(state));
    bind_int64(database_, statement.get(), 2, static_cast<std::int64_t>(limit));
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
        out.push_back(read_grant_row(statement.get()));
    }
    return out;
}

LocalGooseDialogueResponderGrantWrite
LocalGooseDialogueResponderAuthorityStore::prepare_grant(
    const LocalGooseDialogueResponderGrant& grant)
{
    LocalGooseDialogueResponderGrantWrite out;
    if (!authority_schema_ready()) {
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    if (!valid_local_goose_dialogue_responder_grant(grant)
        || grant.state != LocalGooseDialogueResponderGrantState::prepared) {
        out.detail = "invalid initial responder authority grant";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing = ::gaudere_agent::find_grant(database_, grant.grant_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.grant = existing;
            out.detail = "responder authority grant already prepared";
            return out;
        }
        const auto by_request =
            ::gaudere_agent::find_grant_by_request(database_, grant.request_id);
        if (by_request) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.grant = by_request;
            out.detail = "responder authority grant request id conflicts";
            return out;
        }
        Statement statement(database_,
            "INSERT INTO local_goose_dialogue_responder_grant("
            "grant_id,request_id,thread_alias,state,lease_id,created_at_ms,document)"
            "VALUES(?1,?2,?3,0,NULL,?4,?5)");
        bind_text(database_, statement.get(), 1, grant.grant_id);
        bind_text(database_, statement.get(), 2, grant.request_id);
        bind_text(database_, statement.get(), 3, grant.thread_alias);
        bind_int64(database_, statement.get(), 4, grant.created_at_ms);
        bind_text(database_, statement.get(), 5, grant_json(grant).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.grant = grant;
        out.detail = "responder authority grant prepared";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderGrantWrite
LocalGooseDialogueResponderAuthorityStore::activate_grant(
    const std::string& grant_id,
    const std::string& lease_id,
    const std::int64_t activated_at_ms)
{
    LocalGooseDialogueResponderGrantWrite out;
    if (!authority_schema_ready() || !prefixed_sha256(grant_id, grant_prefix)
        || !safe_identifier(lease_id) || activated_at_ms < 0) {
        out.detail = "invalid responder grant activation request";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto grant = ::gaudere_agent::find_grant(database_, grant_id);
        if (!grant) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder authority grant not found";
            return out;
        }
        if (grant->state == LocalGooseDialogueResponderGrantState::activated
            && grant->activated_lease_id
                == std::optional<std::string>{lease_id}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.grant = grant;
            out.detail = "responder authority grant already activated";
            return out;
        }
        if (grant->state != LocalGooseDialogueResponderGrantState::prepared
            || activated_at_ms < grant->created_at_ms
            || activated_at_ms >= grant->expires_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.grant = grant;
            out.detail = "responder authority grant is not activatable";
            return out;
        }
        const auto lease = find_lease_evidence(database_, lease_id);
        if (!lease || lease->state != 0 || lease->turns_committed != 0
            || activated_at_ms < lease->issued_at_ms
            || activated_at_ms >= lease->expires_at_ms
            || !lease_within_grant(*lease, *grant)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.grant = grant;
            out.detail = "responder lease is not admitted by grant envelope";
            return out;
        }
        const auto old_state = grant->state;
        grant->state = LocalGooseDialogueResponderGrantState::activated;
        grant->activated_lease_id = lease_id;
        grant->activated_at_ms = activated_at_ms;
        if (!valid_local_goose_dialogue_responder_grant(*grant)) {
            throw std::runtime_error("activated responder grant became invalid");
        }
        update_grant(database_, *grant, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.grant = grant;
        out.detail = "responder authority grant activated";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderGrantWrite
LocalGooseDialogueResponderAuthorityStore::terminalize_grant(
    const std::string& grant_id,
    const LocalGooseDialogueResponderGrantState terminal_state,
    const std::string& reason,
    const std::int64_t terminal_at_ms)
{
    LocalGooseDialogueResponderGrantWrite out;
    using State = LocalGooseDialogueResponderGrantState;
    if (!authority_schema_ready() || !prefixed_sha256(grant_id, grant_prefix)
        || !(terminal_state == State::revoked || terminal_state == State::expired
             || terminal_state == State::manual_review)
        || reason.empty() || reason.size() > max_reason_bytes
        || terminal_at_ms < 0) {
        out.detail = "invalid responder grant terminal transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto grant = ::gaudere_agent::find_grant(database_, grant_id);
        if (!grant) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder authority grant not found";
            return out;
        }
        if (grant->state == terminal_state && grant->terminal_reason == reason
            && grant->terminal_at_ms
                == std::optional<std::int64_t>{terminal_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.grant = grant;
            out.detail = "responder authority grant already terminal";
            return out;
        }
        if (grant->state != State::prepared
            || terminal_at_ms < grant->created_at_ms
            || (terminal_state == State::expired
                && terminal_at_ms < grant->expires_at_ms)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.grant = grant;
            out.detail = "responder authority grant terminal transition conflicts";
            return out;
        }
        const auto old_state = grant->state;
        grant->state = terminal_state;
        grant->terminal_reason = reason;
        grant->terminal_at_ms = terminal_at_ms;
        if (!valid_local_goose_dialogue_responder_grant(*grant)) {
            throw std::runtime_error("terminal responder grant became invalid");
        }
        update_grant(database_, *grant, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.grant = grant;
        out.detail = "responder authority grant terminalized";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

std::optional<LocalGooseDialogueResponderDecision>
LocalGooseDialogueResponderAuthorityStore::find_decision(
    const std::string& decision_id) const
{
    if (!authority_schema_ready() || !prefixed_sha256(decision_id, decision_prefix)) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_decision(database_, decision_id);
}

std::optional<LocalGooseDialogueResponderDecision>
LocalGooseDialogueResponderAuthorityStore::find_decision_by_request(
    const std::string& request_id) const
{
    if (!authority_schema_ready() || !safe_identifier(request_id)) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_decision_by_request(database_, request_id);
}

std::optional<LocalGooseDialogueResponderDecision>
LocalGooseDialogueResponderAuthorityStore::find_decision_for_event(
    const std::string& consumer_id,
    const std::string& event_id) const
{
    if (!authority_schema_ready() || !safe_identifier(consumer_id)
        || event_id.empty()) {
        return std::nullopt;
    }
    return ::gaudere_agent::find_decision_for_event(
        database_, consumer_id, event_id);
}

std::vector<LocalGooseDialogueResponderDecision>
LocalGooseDialogueResponderAuthorityStore::decisions_in_state(
    const LocalGooseDialogueResponderDecisionState state,
    const std::size_t limit) const
{
    std::vector<LocalGooseDialogueResponderDecision> out;
    if (!authority_schema_ready() || limit == 0 || limit > 1024) return out;
    Statement statement(database_,
        "SELECT request_id,grant_id,lease_id,consumer_id,event_id,state,intent_id,"
        "created_at_ms,document FROM local_goose_dialogue_responder_decision "
        "WHERE state=?1 ORDER BY created_at_ms,decision_id LIMIT ?2");
    bind_int64(database_, statement.get(), 1, static_cast<std::int64_t>(state));
    bind_int64(database_, statement.get(), 2, static_cast<std::int64_t>(limit));
    for (;;) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) break;
        if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database_));
        out.push_back(read_decision_row(statement.get()));
    }
    return out;
}

LocalGooseDialogueResponderDecisionWrite
LocalGooseDialogueResponderAuthorityStore::prepare_decision(
    const LocalGooseDialogueResponderDecision& decision)
{
    LocalGooseDialogueResponderDecisionWrite out;
    if (!authority_schema_ready()) {
        out.detail = "responder authority schema v3 is not enabled";
        return out;
    }
    if (!valid_local_goose_dialogue_responder_decision(decision)
        || decision.state != LocalGooseDialogueResponderDecisionState::prepared) {
        out.detail = "invalid initial responder intervention decision";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto existing =
            ::gaudere_agent::find_decision(database_, decision.decision_id);
        if (existing) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.decision = existing;
            out.detail = "responder intervention decision already prepared";
            return out;
        }
        const auto by_request = ::gaudere_agent::find_decision_by_request(
            database_, decision.request_id);
        if (by_request) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = by_request;
            out.detail = "responder decision request id conflicts";
            return out;
        }
        const auto event_decision = ::gaudere_agent::find_decision_for_event(
            database_, decision.consumer_id, decision.event_id);
        if (event_decision) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = event_decision;
            out.detail = "responder consumer event already has a decision";
            return out;
        }
        const auto grant = ::gaudere_agent::find_grant(database_, decision.grant_id);
        const auto lease = find_lease_evidence(database_, decision.lease_id);
        const auto bit = local_goose_dialogue_responder_message_kind_bit(
            decision.message_kind);
        if (!grant || grant->state != LocalGooseDialogueResponderGrantState::activated
            || grant->activated_lease_id
                != std::optional<std::string>{decision.lease_id}
            || grant->authority_source != decision.authority_source
            || grant->thread_alias != decision.thread_alias
            || grant->speaker_kind != decision.responder_speaker_kind
            || grant->speaker_id != decision.responder_speaker_id
            || (grant->allowed_message_kinds & bit) == 0
            || decision.created_at_ms < grant->created_at_ms
            || decision.created_at_ms >= grant->expires_at_ms
            || !lease || lease->state != 0
            || lease->turns_committed >= lease->max_system_turns
            || decision.created_at_ms < lease->issued_at_ms
            || decision.created_at_ms >= lease->expires_at_ms
            || !lease_within_grant(*lease, *grant)
            || (lease->allowed_message_kinds & bit) == 0) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder decision is not admitted by activated grant/lease";
            return out;
        }
        Statement statement(database_,
            "INSERT INTO local_goose_dialogue_responder_decision("
            "decision_id,request_id,grant_id,lease_id,consumer_id,event_id,state,"
            "intent_id,created_at_ms,document)"
            "VALUES(?1,?2,?3,?4,?5,?6,0,NULL,?7,?8)");
        bind_text(database_, statement.get(), 1, decision.decision_id);
        bind_text(database_, statement.get(), 2, decision.request_id);
        bind_text(database_, statement.get(), 3, decision.grant_id);
        bind_text(database_, statement.get(), 4, decision.lease_id);
        bind_text(database_, statement.get(), 5, decision.consumer_id);
        bind_text(database_, statement.get(), 6, decision.event_id);
        bind_int64(database_, statement.get(), 7, decision.created_at_ms);
        bind_text(database_, statement.get(), 8, decision_json(decision).dump());
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.decision = decision;
        out.detail = "responder intervention decision prepared";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDecisionWrite
LocalGooseDialogueResponderAuthorityStore::bind_decision_intent(
    const std::string& decision_id,
    const std::string& intent_id,
    const std::int64_t bound_at_ms)
{
    LocalGooseDialogueResponderDecisionWrite out;
    if (!authority_schema_ready()
        || !prefixed_sha256(decision_id, decision_prefix)
        || !prefixed_sha256(intent_id, intent_prefix)
        || bound_at_ms < 0) {
        out.detail = "invalid responder decision binding request";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto decision = ::gaudere_agent::find_decision(database_, decision_id);
        if (!decision) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder decision not found";
            return out;
        }
        if (decision->state == LocalGooseDialogueResponderDecisionState::intent_bound
            && decision->intent_id == std::optional<std::string>{intent_id}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.decision = decision;
            out.detail = "responder decision already bound to intent";
            return out;
        }
        if (decision->state != LocalGooseDialogueResponderDecisionState::prepared
            || bound_at_ms < decision->created_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = decision;
            out.detail = "responder decision is not bindable";
            return out;
        }
        const auto grant = ::gaudere_agent::find_grant(database_, decision->grant_id);
        const auto lease = find_lease_evidence(database_, decision->lease_id);
        const auto intent = find_intent_evidence(database_, intent_id);
        if (!grant || grant->state != LocalGooseDialogueResponderGrantState::activated
            || grant->activated_lease_id
                != std::optional<std::string>{decision->lease_id}
            || bound_at_ms >= grant->expires_at_ms
            || !lease || lease->state != 0
            || lease->turns_committed >= lease->max_system_turns
            || bound_at_ms < lease->next_eligible_at_ms
            || bound_at_ms >= lease->expires_at_ms
            || !lease_within_grant(*lease, *grant)
            || !intent || !intent_matches_decision(*intent, *decision, bound_at_ms)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = decision;
            out.detail = "responder decision intent binding evidence conflicts";
            return out;
        }
        const auto old_state = decision->state;
        decision->state = LocalGooseDialogueResponderDecisionState::intent_bound;
        decision->intent_id = intent_id;
        decision->bound_at_ms = bound_at_ms;
        if (!valid_local_goose_dialogue_responder_decision(*decision)) {
            throw std::runtime_error("bound responder decision became invalid");
        }
        update_decision(database_, *decision, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.decision = decision;
        out.detail = "responder decision bound to exact intent";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDecisionWrite
LocalGooseDialogueResponderAuthorityStore::complete_decision(
    const std::string& decision_id,
    const std::int64_t completed_at_ms)
{
    LocalGooseDialogueResponderDecisionWrite out;
    if (!authority_schema_ready()
        || !prefixed_sha256(decision_id, decision_prefix)
        || completed_at_ms < 0) {
        out.detail = "invalid responder decision completion request";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto decision = ::gaudere_agent::find_decision(database_, decision_id);
        if (!decision) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder decision not found";
            return out;
        }
        if (decision->state == LocalGooseDialogueResponderDecisionState::completed
            && decision->completed_at_ms
                == std::optional<std::int64_t>{completed_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.decision = decision;
            out.detail = "responder decision already completed";
            return out;
        }
        if (decision->state != LocalGooseDialogueResponderDecisionState::intent_bound
            || !decision->intent_id || !decision->bound_at_ms
            || completed_at_ms < *decision->bound_at_ms) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = decision;
            out.detail = "responder decision completion conflicts";
            return out;
        }
        const auto intent = find_intent_evidence(database_, *decision->intent_id);
        if (!intent || intent->state != 2
            || !completed_intervention_disposition(
                database_, *decision, *decision->intent_id)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = decision;
            out.detail = "bound responder intervention is not durably completed";
            return out;
        }
        const auto old_state = decision->state;
        decision->state = LocalGooseDialogueResponderDecisionState::completed;
        decision->completed_at_ms = completed_at_ms;
        if (!valid_local_goose_dialogue_responder_decision(*decision)) {
            throw std::runtime_error("completed responder decision became invalid");
        }
        update_decision(database_, *decision, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.decision = decision;
        out.detail = "responder decision completed";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueResponderDecisionWrite
LocalGooseDialogueResponderAuthorityStore::terminalize_decision(
    const std::string& decision_id,
    const LocalGooseDialogueResponderDecisionState terminal_state,
    const std::string& reason,
    const std::int64_t terminal_at_ms)
{
    LocalGooseDialogueResponderDecisionWrite out;
    using State = LocalGooseDialogueResponderDecisionState;
    if (!authority_schema_ready()
        || !prefixed_sha256(decision_id, decision_prefix)
        || !(terminal_state == State::conflict || terminal_state == State::expired
             || terminal_state == State::manual_review)
        || reason.empty() || reason.size() > max_reason_bytes
        || terminal_at_ms < 0) {
        out.detail = "invalid responder decision terminal transition";
        return out;
    }
    try {
        execute(database_, "BEGIN IMMEDIATE;");
        auto decision = ::gaudere_agent::find_decision(database_, decision_id);
        if (!decision) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.detail = "responder decision not found";
            return out;
        }
        if (decision->state == terminal_state
            && decision->terminal_reason == reason
            && decision->terminal_at_ms
                == std::optional<std::int64_t>{terminal_at_ms}) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::duplicate;
            out.decision = decision;
            out.detail = "responder decision already terminal";
            return out;
        }
        if (!(decision->state == State::prepared
              || decision->state == State::intent_bound)
            || terminal_at_ms < decision->created_at_ms
            || (decision->bound_at_ms
                && terminal_at_ms < *decision->bound_at_ms)) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueResponderStoreResult::conflict;
            out.decision = decision;
            out.detail = "responder decision terminal transition conflicts";
            return out;
        }
        const auto old_state = decision->state;
        decision->state = terminal_state;
        decision->terminal_reason = reason;
        decision->terminal_at_ms = terminal_at_ms;
        if (!valid_local_goose_dialogue_responder_decision(*decision)) {
            throw std::runtime_error("terminal responder decision became invalid");
        }
        update_decision(database_, *decision, old_state);
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueResponderStoreResult::accepted;
        out.decision = decision;
        out.detail = "responder decision terminalized";
        return out;
    } catch (const std::exception& error) {
        try { execute(database_, "ROLLBACK;"); } catch (...) {}
        out.result = LocalGooseDialogueResponderStoreResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

} // namespace gaudere_agent
