#include "LocalGooseDialogueCompletionFeed.hpp"

#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>
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
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_alias_bytes = 128;
constexpr std::size_t max_consumer_id_bytes = 128;
constexpr std::size_t max_request_id_bytes = 128;
constexpr std::size_t max_speaker_id_bytes = 128;
constexpr std::size_t max_response_bytes = 16 * 1024;
constexpr const char* v2_prefix = "cognition.local-goose-dialogue.v2:";
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
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
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

bool dialogue_task_id(const std::string& value) noexcept
{
    return prefixed_sha256(value, v2_prefix)
        || prefixed_sha256(value, v3_prefix);
}

bool safe_identifier(
    const std::string& value,
    const std::size_t max_bytes) noexcept
{
    if (value.empty() || value.size() > max_bytes) return false;
    for (const unsigned char c : value) {
        const bool ok =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool valid_speaker_kind(const std::string& value) noexcept
{
    return value == "human" || value == "system";
}

bool valid_message_kind(const std::string& value) noexcept
{
    return value == "dialogue"
        || value == "feedback"
        || value == "intervention"
        || value == "observation";
}

void bind_text(
    sqlite3* database,
    sqlite3_stmt* statement,
    const int index,
    const std::string& value)
{
    if (sqlite3_bind_text64(
            statement, index, value.data(), value.size(),
            SQLITE_TRANSIENT, SQLITE_UTF8) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

std::string text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(
        reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(bytes));
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
            "cannot stat Local Goose dialogue completion feed sidecar");
    }
    if (!S_ISREG(status.st_mode)
        || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "Local Goose dialogue completion feed sidecar must be a "
            "current-user regular file mode 0600");
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
            "cannot securely create Local Goose dialogue completion feed sidecar");
    }
    verify_owner_mode(path);
}

std::string completion_event_id(
    const std::string& thread_alias,
    const std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& result_sha256)
{
    const Json identity{
        {"result_sha256", result_sha256},
        {"schema", "gaudere.cognition.local-goose-dialogue.completion.identity.v1"},
        {"task_id", task_id},
        {"thread_alias", thread_alias},
        {"thread_revision", thread_revision}
    };
    return std::string{local_goose_dialogue_completion_event_prefix}
        + sha256_hex(identity.dump());
}

bool same_event_identity(
    const LocalGooseDialogueCompletionEvent& left,
    const LocalGooseDialogueCompletionEvent& right) noexcept
{
    return left.event_id == right.event_id
        && left.thread_alias == right.thread_alias
        && left.thread_revision == right.thread_revision
        && left.task_id == right.task_id
        && left.root_task_id == right.root_task_id
        && left.turn_index == right.turn_index
        && left.request_id == right.request_id
        && left.speaker_kind == right.speaker_kind
        && left.speaker_id == right.speaker_id
        && left.message_kind == right.message_kind
        && left.result_sha256 == right.result_sha256
        && left.response == right.response;
}

LocalGooseDialogueCompletionEvent read_event(sqlite3_stmt* statement)
{
    const auto sequence = sqlite3_column_int64(statement, 0);
    const auto thread_revision = sqlite3_column_int64(statement, 3);
    const auto turn_index = sqlite3_column_int64(statement, 6);
    const auto observed_completed_at_ms = sqlite3_column_int64(statement, 13);
    if (sequence <= 0 || thread_revision < 0 || turn_index < 0
        || observed_completed_at_ms < 0) {
        throw std::runtime_error(
            "invalid Local Goose dialogue completion event integer field");
    }

    LocalGooseDialogueCompletionEvent event;
    event.sequence = static_cast<std::uint64_t>(sequence);
    event.event_id = text(statement, 1);
    event.thread_alias = text(statement, 2);
    event.thread_revision = static_cast<std::uint64_t>(thread_revision);
    event.task_id = text(statement, 4);
    event.root_task_id = text(statement, 5);
    event.turn_index = static_cast<std::uint64_t>(turn_index);
    event.request_id = text(statement, 7);
    event.speaker_kind = text(statement, 8);
    event.speaker_id = text(statement, 9);
    event.message_kind = text(statement, 10);
    event.result_sha256 = text(statement, 11);
    event.response = text(statement, 12);
    event.observed_completed_at_ms = observed_completed_at_ms;
    if (!valid_local_goose_dialogue_completion_event(event)) {
        throw std::runtime_error(
            "non-canonical Local Goose dialogue completion event row");
    }
    return event;
}

constexpr const char* event_columns =
    "sequence,event_id,thread_alias,thread_revision,task_id,root_task_id,"
    "turn_index,request_id,speaker_kind,speaker_id,message_kind,"
    "result_sha256,response,observed_completed_at_ms";

std::optional<LocalGooseDialogueCompletionEvent> find_event_for_revision(
    sqlite3* database,
    const std::string& thread_alias,
    const std::uint64_t thread_revision)
{
    Statement statement(
        database,
        "SELECT sequence,event_id,thread_alias,thread_revision,task_id,"
        "root_task_id,turn_index,request_id,speaker_kind,speaker_id,"
        "message_kind,result_sha256,response,observed_completed_at_ms "
        "FROM local_goose_dialogue_completion_event "
        "WHERE thread_alias=?1 AND thread_revision=?2");
    bind_text(database, statement.get(), 1, thread_alias);
    if (sqlite3_bind_int64(
            statement.get(), 2,
            static_cast<sqlite3_int64>(thread_revision)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto event = read_event(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "duplicate Local Goose dialogue completion revision row");
    }
    return event;
}

std::optional<std::uint64_t> materialization_cursor(
    sqlite3* database,
    const std::string& thread_alias)
{
    Statement statement(
        database,
        "SELECT next_revision FROM local_goose_dialogue_materialization "
        "WHERE thread_alias=?1");
    bind_text(database, statement.get(), 1, thread_alias);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    const auto value = sqlite3_column_int64(statement.get(), 0);
    if (value < 0) {
        throw std::runtime_error(
            "invalid Local Goose dialogue materialization cursor");
    }
    return static_cast<std::uint64_t>(value);
}

std::uint64_t consumer_cursor(
    sqlite3* database,
    const std::string& consumer_id)
{
    Statement statement(
        database,
        "SELECT last_sequence FROM local_goose_dialogue_consumer_cursor "
        "WHERE consumer_id=?1");
    bind_text(database, statement.get(), 1, consumer_id);
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return 0;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    const auto value = sqlite3_column_int64(statement.get(), 0);
    if (value < 0) {
        throw std::runtime_error(
            "invalid Local Goose dialogue consumer cursor");
    }
    return static_cast<std::uint64_t>(value);
}

std::optional<LocalGooseDialogueCompletionEvent> first_event_after(
    sqlite3* database,
    const std::uint64_t sequence)
{
    Statement statement(
        database,
        "SELECT sequence,event_id,thread_alias,thread_revision,task_id,"
        "root_task_id,turn_index,request_id,speaker_kind,speaker_id,"
        "message_kind,result_sha256,response,observed_completed_at_ms "
        "FROM local_goose_dialogue_completion_event "
        "WHERE sequence>?1 ORDER BY sequence ASC LIMIT 1");
    if (sqlite3_bind_int64(
            statement.get(), 1,
            static_cast<sqlite3_int64>(sequence)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) return std::nullopt;
    if (step != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    return read_event(statement.get());
}

void write_materialization_cursor(
    sqlite3* database,
    const std::string& thread_alias,
    const std::uint64_t next_revision)
{
    Statement statement(
        database,
        "INSERT INTO local_goose_dialogue_materialization"
        "(thread_alias,next_revision) VALUES(?1,?2) "
        "ON CONFLICT(thread_alias) DO UPDATE SET "
        "next_revision=excluded.next_revision");
    bind_text(database, statement.get(), 1, thread_alias);
    if (sqlite3_bind_int64(
            statement.get(), 2,
            static_cast<sqlite3_int64>(next_revision)) != SQLITE_OK
        || sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
}

} // namespace

bool valid_local_goose_dialogue_completion_event(
    const LocalGooseDialogueCompletionEvent& event) noexcept
{
    return event.sequence <= static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max())
        && safe_identifier(event.thread_alias, max_alias_bytes)
        && event.thread_revision < static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        && prefixed_sha256(
            event.event_id, local_goose_dialogue_completion_event_prefix)
        && dialogue_task_id(event.task_id)
        && dialogue_task_id(event.root_task_id)
        && event.turn_index <= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())
        && safe_identifier(event.request_id, max_request_id_bytes)
        && valid_speaker_kind(event.speaker_kind)
        && safe_identifier(event.speaker_id, max_speaker_id_bytes)
        && valid_message_kind(event.message_kind)
        && lowercase_sha256(event.result_sha256)
        && !event.response.empty()
        && event.response.size() <= max_response_bytes
        && event.observed_completed_at_ms >= 0
        && event.event_id == completion_event_id(
            event.thread_alias, event.thread_revision,
            event.task_id, event.result_sha256);
}

LocalGooseDialogueCompletionEvent
make_local_goose_dialogue_completion_event(
    const std::string& thread_alias,
    const std::uint64_t thread_revision,
    const gaudere::work::Task& task,
    const std::int64_t observed_completed_at_ms)
{
    LocalGooseDialogueCompletionEvent event;
    event.thread_alias = thread_alias;
    event.thread_revision = thread_revision;
    event.task_id = task.id;
    event.observed_completed_at_ms = observed_completed_at_ms;

    if (canonical_local_goose_dialogue_v3_success(task) && task.result) {
        const auto request = inspect_local_goose_dialogue_v3_task(task);
        const auto result = inspect_local_goose_dialogue_v3_response(
            task, task.result->output);
        if (!request.eligible || !result.eligible) {
            throw std::invalid_argument(
                "canonical V3 dialogue inspection failed");
        }
        event.root_task_id = result.root_task_id;
        event.turn_index = result.turn_index;
        event.request_id = result.request_id;
        event.speaker_kind = result.speaker_kind;
        event.speaker_id = result.speaker_id;
        event.message_kind = result.message_kind;
        event.response = result.response;
    } else if (canonical_local_goose_dialogue_v2_success(task) && task.result) {
        const auto request = inspect_local_goose_dialogue_v2_task(task);
        const auto result = inspect_local_goose_dialogue_v2_response(
            task, task.result->output);
        if (!request.eligible || !result.eligible) {
            throw std::invalid_argument(
                "canonical V2 dialogue inspection failed");
        }
        event.root_task_id = result.root_task_id;
        event.turn_index = result.turn_index;
        event.request_id = result.request_id;
        event.speaker_kind = "human";
        event.speaker_id = "legacy-v2-human";
        event.message_kind = "dialogue";
        event.response = result.response;
    } else {
        throw std::invalid_argument(
            "dialogue completion event requires canonical successful V2/V3 Task");
    }

    event.result_sha256 = sha256_hex(task.result->output);
    event.event_id = completion_event_id(
        event.thread_alias, event.thread_revision,
        event.task_id, event.result_sha256);
    if (!valid_local_goose_dialogue_completion_event(event)) {
        throw std::invalid_argument(
            "dialogue completion event is not canonical");
    }
    return event;
}

LocalGooseDialogueCompletionFeedStore::LocalGooseDialogueCompletionFeedStore(
    const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(
            path.c_str(), &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_)
            : "cannot open Local Goose dialogue completion feed SQLite";
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
        const int version = sqlite3_column_int(version_statement.get(), 0);
        if (version > local_goose_dialogue_completion_feed_schema) {
            throw std::runtime_error(
                "unsupported Local Goose dialogue completion feed schema");
        }
        if (version == 0) {
            if (user_table_count(database_) != 0) {
                throw std::runtime_error(
                    "unversioned dialogue completion feed is not empty");
            }
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_completion_event ("
                " sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
                " event_id TEXT NOT NULL UNIQUE,"
                " thread_alias TEXT NOT NULL,"
                " thread_revision INTEGER NOT NULL CHECK(thread_revision >= 0),"
                " task_id TEXT NOT NULL,"
                " root_task_id TEXT NOT NULL,"
                " turn_index INTEGER NOT NULL CHECK(turn_index >= 0),"
                " request_id TEXT NOT NULL,"
                " speaker_kind TEXT NOT NULL,"
                " speaker_id TEXT NOT NULL,"
                " message_kind TEXT NOT NULL,"
                " result_sha256 TEXT NOT NULL,"
                " response TEXT NOT NULL,"
                " observed_completed_at_ms INTEGER NOT NULL "
                    "CHECK(observed_completed_at_ms >= 0),"
                " UNIQUE(thread_alias,thread_revision)"
                ");");
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_materialization ("
                " thread_alias TEXT PRIMARY KEY NOT NULL,"
                " next_revision INTEGER NOT NULL CHECK(next_revision >= 0)"
                ");");
            execute(
                database_,
                "CREATE TABLE local_goose_dialogue_consumer_cursor ("
                " consumer_id TEXT PRIMARY KEY NOT NULL,"
                " last_sequence INTEGER NOT NULL CHECK(last_sequence >= 0)"
                ");");
            execute(database_, "PRAGMA user_version=1;");
        } else if (user_table_count(database_) != 3) {
            throw std::runtime_error(
                "Local Goose dialogue completion feed table set differs");
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

LocalGooseDialogueCompletionFeedStore::~LocalGooseDialogueCompletionFeedStore()
{
    sqlite3_close(database_);
}

std::optional<std::uint64_t>
LocalGooseDialogueCompletionFeedStore::materialization_next_revision(
    const std::string& thread_alias) const
{
    if (!safe_identifier(thread_alias, max_alias_bytes)) return std::nullopt;
    return materialization_cursor(database_, thread_alias);
}

LocalGooseDialogueCompletionFeedWrite
LocalGooseDialogueCompletionFeedStore::append_for_revision(
    const LocalGooseDialogueCompletionEvent& event)
{
    LocalGooseDialogueCompletionFeedWrite out;
    if (!valid_local_goose_dialogue_completion_event(event)
        || event.sequence != 0) {
        out.detail = "invalid dialogue completion event append";
        return out;
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto current =
            materialization_cursor(database_, event.thread_alias);
        const auto existing = find_event_for_revision(
            database_, event.thread_alias, event.thread_revision);

        if (existing) {
            if (!same_event_identity(*existing, event)) {
                execute(database_, "COMMIT;");
                out.result = LocalGooseDialogueCompletionFeedResult::conflict;
                out.event = existing;
                out.detail =
                    "dialogue completion revision already has different event";
                return out;
            }
            if (current && *current < event.thread_revision) {
                execute(database_, "COMMIT;");
                out.result = LocalGooseDialogueCompletionFeedResult::conflict;
                out.event = existing;
                out.detail =
                    "dialogue completion materialization cursor is behind event";
                return out;
            }
            if (!current || *current == event.thread_revision) {
                write_materialization_cursor(
                    database_, event.thread_alias,
                    event.thread_revision + 1);
            }
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueCompletionFeedResult::duplicate;
            out.event = existing;
            out.detail = "dialogue completion event already materialized";
            return out;
        }

        if (current && *current != event.thread_revision) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueCompletionFeedResult::conflict;
            out.detail =
                "dialogue completion materialization revision conflict";
            return out;
        }

        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_completion_event("
            "event_id,thread_alias,thread_revision,task_id,root_task_id,"
            "turn_index,request_id,speaker_kind,speaker_id,message_kind,"
            "result_sha256,response,observed_completed_at_ms)"
            " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)");
        bind_text(database_, statement.get(), 1, event.event_id);
        bind_text(database_, statement.get(), 2, event.thread_alias);
        if (sqlite3_bind_int64(
                statement.get(), 3,
                static_cast<sqlite3_int64>(event.thread_revision)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(database_, statement.get(), 4, event.task_id);
        bind_text(database_, statement.get(), 5, event.root_task_id);
        if (sqlite3_bind_int64(
                statement.get(), 6,
                static_cast<sqlite3_int64>(event.turn_index)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(database_, statement.get(), 7, event.request_id);
        bind_text(database_, statement.get(), 8, event.speaker_kind);
        bind_text(database_, statement.get(), 9, event.speaker_id);
        bind_text(database_, statement.get(), 10, event.message_kind);
        bind_text(database_, statement.get(), 11, event.result_sha256);
        bind_text(database_, statement.get(), 12, event.response);
        if (sqlite3_bind_int64(
                statement.get(), 13,
                static_cast<sqlite3_int64>(
                    event.observed_completed_at_ms)) != SQLITE_OK
            || sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }

        const auto sequence = sqlite3_last_insert_rowid(database_);
        if (sequence <= 0) {
            throw std::runtime_error(
                "dialogue completion event has invalid sequence");
        }
        write_materialization_cursor(
            database_, event.thread_alias, event.thread_revision + 1);
        execute(database_, "COMMIT;");

        auto stored = event;
        stored.sequence = static_cast<std::uint64_t>(sequence);
        out.result = LocalGooseDialogueCompletionFeedResult::accepted;
        out.event = std::move(stored);
        out.detail = "dialogue completion event materialized";
        return out;
    } catch (const std::exception& error) {
        try {
            execute(database_, "ROLLBACK;");
        } catch (...) {
        }
        out.result = LocalGooseDialogueCompletionFeedResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

std::optional<LocalGooseDialogueCompletionEvent>
LocalGooseDialogueCompletionFeedStore::next_for_consumer(
    const std::string& consumer_id) const
{
    if (!safe_identifier(consumer_id, max_consumer_id_bytes)) {
        return std::nullopt;
    }
    const auto last = consumer_cursor(database_, consumer_id);
    return first_event_after(database_, last);
}

LocalGooseDialogueCompletionCursorWrite
LocalGooseDialogueCompletionFeedStore::acknowledge(
    const std::string& consumer_id,
    const std::uint64_t sequence)
{
    LocalGooseDialogueCompletionCursorWrite out;
    if (!safe_identifier(consumer_id, max_consumer_id_bytes)
        || sequence == 0
        || sequence > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        out.detail = "invalid dialogue completion consumer acknowledgement";
        return out;
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto current = consumer_cursor(database_, consumer_id);
        if (current == sequence) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueCompletionFeedResult::duplicate;
            out.last_sequence = current;
            out.detail = "dialogue completion event already acknowledged";
            return out;
        }
        if (current > sequence) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueCompletionFeedResult::conflict;
            out.last_sequence = current;
            out.detail = "dialogue completion acknowledgement is stale";
            return out;
        }
        const auto next = first_event_after(database_, current);
        if (!next || next->sequence != sequence) {
            execute(database_, "COMMIT;");
            out.result = LocalGooseDialogueCompletionFeedResult::conflict;
            out.last_sequence = current;
            out.detail =
                "dialogue completion acknowledgement would skip an event";
            return out;
        }

        Statement statement(
            database_,
            "INSERT INTO local_goose_dialogue_consumer_cursor"
            "(consumer_id,last_sequence) VALUES(?1,?2) "
            "ON CONFLICT(consumer_id) DO UPDATE SET "
            "last_sequence=excluded.last_sequence");
        bind_text(database_, statement.get(), 1, consumer_id);
        if (sqlite3_bind_int64(
                statement.get(), 2,
                static_cast<sqlite3_int64>(sequence)) != SQLITE_OK
            || sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        out.result = LocalGooseDialogueCompletionFeedResult::accepted;
        out.last_sequence = sequence;
        out.detail = "dialogue completion event acknowledged";
        return out;
    } catch (const std::exception& error) {
        try {
            execute(database_, "ROLLBACK;");
        } catch (...) {
        }
        out.result = LocalGooseDialogueCompletionFeedResult::unavailable;
        out.detail = error.what();
        return out;
    }
}

LocalGooseDialogueCompletionFeedService::LocalGooseDialogueCompletionFeedService(
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueCompletionFeedStore& feed_store,
    LocalGooseDialogueCompletionTaskLookup lookup,
    LocalGooseDialogueCompletionClock clock)
    : thread_store_(thread_store),
      feed_store_(feed_store),
      lookup_(std::move(lookup)),
      clock_(std::move(clock))
{
    if (!lookup_ || !clock_) {
        throw std::invalid_argument(
            "dialogue completion feed lookup and clock are required");
    }
}

LocalGooseDialogueCompletionReconcileResult
LocalGooseDialogueCompletionFeedService::reconcile(
    const std::string& thread_alias,
    const std::size_t limit)
{
    LocalGooseDialogueCompletionReconcileResult out;
    if (!safe_identifier(thread_alias, max_alias_bytes)
        || limit == 0 || limit > 1024) {
        out.blocked = true;
        out.detail = "invalid dialogue completion reconciliation request";
        return out;
    }

    const auto materialization =
        feed_store_.materialization_next_revision(thread_alias);
    const auto first_revision = materialization.value_or(0);
    const auto history =
        thread_store_.history_from(thread_alias, first_revision, limit);
    if (history.empty()) {
        if (!thread_store_.find(thread_alias)) {
            out.blocked = true;
            out.detail = "dialogue completion thread alias not found";
        } else {
            out.detail = "dialogue completion feed is caught up";
        }
        return out;
    }

    std::optional<std::uint64_t> expected = materialization;
    for (const auto& head : history) {
        if (expected && head.revision != *expected) {
            out.blocked = true;
            out.detail = "dialogue completion thread revision journal has a gap";
            return out;
        }

        const auto task = lookup_(head.head_task_id);
        if (!task) {
            out.blocked = true;
            out.detail = "dialogue completion head Task not found";
            return out;
        }

        LocalGooseDialogueCompletionEvent event;
        try {
            event = make_local_goose_dialogue_completion_event(
                thread_alias, head.revision, *task, clock_());
        } catch (const std::invalid_argument& error) {
            if (gaudere::work::is_terminal(task->status)) {
                out.blocked = true;
                out.detail = std::string(
                    "dialogue completion head is terminal without canonical response: ")
                    + error.what();
            } else {
                out.pending = true;
                out.detail =
                    "dialogue completion head has not reached canonical success";
            }
            return out;
        }

        if (event.task_id != head.head_task_id
            || event.root_task_id != head.root_task_id) {
            out.blocked = true;
            out.detail =
                "dialogue completion event differs from preferred thread journal";
            return out;
        }

        const auto write = feed_store_.append_for_revision(event);
        if (write.result != LocalGooseDialogueCompletionFeedResult::accepted
            && write.result
                != LocalGooseDialogueCompletionFeedResult::duplicate) {
            out.blocked = true;
            out.detail =
                "dialogue completion feed materialization failed: "
                + write.detail;
            return out;
        }
        ++out.materialized;
        expected = head.revision + 1;
    }

    out.detail = "dialogue completion feed reconciled";
    return out;
}

} // namespace gaudere_agent
