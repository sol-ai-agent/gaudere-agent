#include "GooseGovernanceStore.hpp"

#include "Sha256.hpp"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t risk_max_calls = 32;

const std::vector<std::string> runtime_tools = {
    "gaudere_inspect_budget",
    "gaudere_inspect_task",
    "gaudere_inspect_wake_status",
    "gaudere_local_echo",
    "gaudere_request_openai_risk_review"
};

void require_sqlite(const int code, sqlite3* db, const char* operation)
{
    if (code == SQLITE_OK || code == SQLITE_DONE || code == SQLITE_ROW) return;
    throw std::runtime_error(std::string(operation) + ": "
                             + (db ? sqlite3_errmsg(db) : "sqlite error"));
}

void exec(sqlite3* db, const char* sql)
{
    char* error = nullptr;
    const int code = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (code != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        throw std::runtime_error("governance sqlite: " + message);
    }
}

std::string enabled_tools_json(const std::vector<std::string>& tools)
{
    return Json(tools).dump();
}

std::vector<std::string> parse_enabled_tools(const std::string& raw)
{
    const auto value = Json::parse(raw);
    if (!value.is_array()) throw std::runtime_error("operational tools are not an array");
    std::vector<std::string> tools;
    for (const auto& item : value) {
        if (!item.is_string()) throw std::runtime_error("operational tool name is not a string");
        tools.push_back(item.get<std::string>());
    }
    return tools;
}

bool known_runtime_tool(const std::string& name)
{
    return std::find(runtime_tools.begin(), runtime_tools.end(), name)
        != runtime_tools.end();
}

void validate_operational_policy(const std::vector<std::string>& tools,
                                 const std::size_t max_calls)
{
    if (max_calls == 0 || max_calls > risk_max_calls) {
        throw std::invalid_argument("operational max_calls exceeds current risk envelope");
    }
    std::vector<std::string> sorted = tools;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::invalid_argument("operational tool list contains duplicates");
    }
    for (const auto& tool : tools) {
        if (!known_runtime_tool(tool)) {
            throw std::invalid_argument("operational policy requests a tool outside current risk envelope");
        }
    }
}

void validate_text(const std::string& value,
                   const std::size_t max_bytes,
                   const char* label)
{
    if (value.empty() || value.size() > max_bytes) {
        throw std::invalid_argument(std::string(label) + " must be non-empty and bounded");
    }
    for (const unsigned char c : value) {
        if (c == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) {
            throw std::invalid_argument(std::string(label) + " contains invalid control characters");
        }
    }
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

GooseRiskProposal proposal_from_statement(sqlite3_stmt* statement)
{
    GooseRiskProposal proposal;
    proposal.id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    proposal.proposal = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    proposal.reason = reinterpret_cast<const char*>(sqlite3_column_text(statement, 2));
    proposal.status = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
    if (sqlite3_column_type(statement, 4) != SQLITE_NULL) {
        proposal.review_task_id = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 4));
    }
    return proposal;
}

} // namespace

GooseGovernanceStore::GooseGovernanceStore(std::string path)
    : path_(std::move(path))
{
    if (path_.empty() || path_.front() != '/') {
        throw std::invalid_argument("Goose governance sidecar path must be absolute");
    }
    const int code = sqlite3_open_v2(
        path_.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (code != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "cannot open sqlite database";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open Goose governance sidecar: " + message);
    }
    sqlite3_busy_timeout(db_, 2000);
    if (::chmod(path_.c_str(), 0600) != 0) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot enforce Goose governance sidecar mode 0600");
    }
    initialize();
}

GooseGovernanceStore::~GooseGovernanceStore()
{
    if (db_) sqlite3_close(db_);
}

void GooseGovernanceStore::initialize()
{
    exec(db_, "PRAGMA foreign_keys=ON");
    exec(db_,
         "CREATE TABLE IF NOT EXISTS goose_operational_policy ("
         " singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
         " revision INTEGER NOT NULL CHECK(revision>=0),"
         " enabled_tools TEXT NOT NULL,"
         " max_calls INTEGER NOT NULL CHECK(max_calls>0),"
         " updated_at_ms INTEGER NOT NULL"
         ")");
    exec(db_,
         "CREATE TABLE IF NOT EXISTS goose_risk_proposals ("
         " id TEXT PRIMARY KEY NOT NULL,"
         " proposal TEXT NOT NULL,"
         " reason TEXT NOT NULL,"
         " status TEXT NOT NULL CHECK(status IN ('pending_openai','review_submitted')) ,"
         " review_task_id TEXT NULL,"
         " created_at_ms INTEGER NOT NULL"
         ")");

    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "INSERT OR IGNORE INTO goose_operational_policy"
                       "(singleton,revision,enabled_tools,max_calls,updated_at_ms)"
                       " VALUES(1,0,?,?,?)",
                       -1, &statement, nullptr),
                   db_, "prepare default Goose operational policy");
    const auto tools = enabled_tools_json(runtime_tools);
    sqlite3_bind_text(statement, 1, tools.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, 8);
    sqlite3_bind_int64(statement, 3, now_ms());
    require_sqlite(sqlite3_step(statement), db_, "seed default Goose operational policy");
    sqlite3_finalize(statement);
}

GooseOperationalPolicy GooseGovernanceStore::operational_policy() const
{
    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "SELECT revision,enabled_tools,max_calls FROM goose_operational_policy"
                       " WHERE singleton=1",
                       -1, &statement, nullptr),
                   db_, "prepare Goose operational policy read");
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error("Goose operational policy singleton is missing");
    }
    GooseOperationalPolicy policy;
    policy.revision = static_cast<std::uint64_t>(sqlite3_column_int64(statement, 0));
    policy.enabled_tools = parse_enabled_tools(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
    policy.max_calls_per_cycle = static_cast<std::size_t>(
        sqlite3_column_int64(statement, 2));
    sqlite3_finalize(statement);
    validate_operational_policy(policy.enabled_tools, policy.max_calls_per_cycle);
    return policy;
}

GooseOperationalPolicy GooseGovernanceStore::set_operational_policy(
    const std::vector<std::string>& enabled_tools,
    const std::size_t max_calls_per_cycle)
{
    validate_operational_policy(enabled_tools, max_calls_per_cycle);
    const auto current = operational_policy();
    const auto encoded = enabled_tools_json(enabled_tools);

    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "UPDATE goose_operational_policy"
                       " SET revision=?,enabled_tools=?,max_calls=?,updated_at_ms=?"
                       " WHERE singleton=1 AND revision=?",
                       -1, &statement, nullptr),
                   db_, "prepare Goose operational policy update");
    sqlite3_bind_int64(statement, 1, static_cast<sqlite3_int64>(current.revision + 1));
    sqlite3_bind_text(statement, 2, encoded.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 3, static_cast<sqlite3_int64>(max_calls_per_cycle));
    sqlite3_bind_int64(statement, 4, now_ms());
    sqlite3_bind_int64(statement, 5, static_cast<sqlite3_int64>(current.revision));
    require_sqlite(sqlite3_step(statement), db_, "update Goose operational policy");
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(statement);
    if (changed != 1) throw std::runtime_error("Goose operational policy update conflicted");
    return operational_policy();
}

GooseRiskProposal GooseGovernanceStore::propose_risk_rule_change(
    const std::string& proposal,
    const std::string& reason)
{
    validate_text(proposal, 2048, "risk proposal");
    validate_text(reason, 1024, "risk proposal reason");
    const auto canonical = Json{{"proposal", proposal},
                                {"reason", reason},
                                {"schema", "gaudere.governance.risk-proposal.v1"}}
                               .dump();
    const std::string id = "risk-proposal.v1:" + sha256_hex(canonical);

    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "INSERT OR IGNORE INTO goose_risk_proposals"
                       "(id,proposal,reason,status,review_task_id,created_at_ms)"
                       " VALUES(?,?,?,'pending_openai',NULL,?)",
                       -1, &statement, nullptr),
                   db_, "prepare Goose risk proposal insert");
    sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, proposal.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, now_ms());
    require_sqlite(sqlite3_step(statement), db_, "insert Goose risk proposal");
    sqlite3_finalize(statement);

    const auto found = find_risk_proposal(id);
    if (!found) throw std::runtime_error("Goose risk proposal disappeared after insert");
    return *found;
}

std::optional<GooseRiskProposal> GooseGovernanceStore::find_risk_proposal(
    const std::string& id) const
{
    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "SELECT id,proposal,reason,status,review_task_id"
                       " FROM goose_risk_proposals WHERE id=?",
                       -1, &statement, nullptr),
                   db_, "prepare Goose risk proposal lookup");
    sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    const int step = sqlite3_step(statement);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return std::nullopt;
    }
    require_sqlite(step, db_, "read Goose risk proposal");
    auto proposal = proposal_from_statement(statement);
    sqlite3_finalize(statement);
    return proposal;
}

std::vector<GooseRiskProposal> GooseGovernanceStore::list_risk_proposals() const
{
    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "SELECT id,proposal,reason,status,review_task_id"
                       " FROM goose_risk_proposals ORDER BY created_at_ms,id",
                       -1, &statement, nullptr),
                   db_, "prepare Goose risk proposal list");
    std::vector<GooseRiskProposal> proposals;
    for (;;) {
        const int step = sqlite3_step(statement);
        if (step == SQLITE_DONE) break;
        require_sqlite(step, db_, "list Goose risk proposals");
        proposals.push_back(proposal_from_statement(statement));
    }
    sqlite3_finalize(statement);
    return proposals;
}

bool GooseGovernanceStore::mark_risk_review_submitted(
    const std::string& id,
    const std::string& review_task_id)
{
    validate_text(review_task_id, 128, "risk review task id");
    sqlite3_stmt* statement = nullptr;
    require_sqlite(sqlite3_prepare_v2(
                       db_,
                       "UPDATE goose_risk_proposals"
                       " SET status='review_submitted',review_task_id=?"
                       " WHERE id=? AND status='pending_openai'",
                       -1, &statement, nullptr),
                   db_, "prepare Goose risk review submission mark");
    sqlite3_bind_text(statement, 1, review_task_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    require_sqlite(sqlite3_step(statement), db_, "mark Goose risk review submitted");
    const int changed = sqlite3_changes(db_);
    sqlite3_finalize(statement);
    if (changed == 1) return true;

    const auto found = find_risk_proposal(id);
    return found && found->status == "review_submitted"
        && found->review_task_id && *found->review_task_id == review_task_id;
}

const std::vector<std::string>& GooseGovernanceStore::runtime_tool_envelope()
{
    return runtime_tools;
}

std::size_t GooseGovernanceStore::risk_max_calls_per_cycle() noexcept
{
    return risk_max_calls;
}

} // namespace gaudere_agent
