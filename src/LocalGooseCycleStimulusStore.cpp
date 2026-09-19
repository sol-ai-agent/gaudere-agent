#include "LocalGooseCycleStimulusStore.hpp"

#include "Sha256.hpp"

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

namespace gaudere_agent {
namespace {

constexpr std::size_t max_source_id_bytes = 128;
constexpr std::size_t max_terminal_reason_bytes = 1024;
constexpr std::size_t max_inspection_records = 1024;

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

bool safe_reason(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_terminal_reason_bytes) return false;
    for (const unsigned char character : value) {
        if (character < 0x20u || character == 0x7fu) return false;
    }
    return true;
}

bool safe_source_id(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_source_id_bytes) return false;
    for (const unsigned char character : value) {
        const bool allowed =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '.' || character == '_' || character == ':'
            || character == '-';
        if (!allowed) return false;
    }
    return true;
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

bool valid_stimulus_id(const std::string& value) noexcept
{
    const std::string prefix{local_goose_cycle_stimulus_id_prefix};
    return value.size() == prefix.size() + 64
        && value.compare(0, prefix.size(), prefix) == 0
        && lowercase_sha256(value.substr(prefix.size()));
}

std::string identity_material(const std::string& source_id,
                              const std::uint64_t target_cycle_revision,
                              const std::uint64_t target_cycle_generation)
{
    return std::string{local_goose_cycle_stimulus_scope} + "\n"
        + local_goose_cycle_explicit_recheck_source + "\n"
        + source_id + "\n"
        + std::to_string(target_cycle_revision) + "\n"
        + std::to_string(target_cycle_generation);
}

bool same_stimulus(const LocalGooseCycleStimulus& left,
                   const LocalGooseCycleStimulus& right) noexcept
{
    return left.scope == right.scope
        && left.id == right.id
        && left.source_kind == right.source_kind
        && left.source_id == right.source_id
        && left.accepted_at_ms == right.accepted_at_ms
        && left.target_cycle_revision == right.target_cycle_revision
        && left.target_cycle_generation == right.target_cycle_generation
        && left.status == right.status
        && left.terminal_at_ms == right.terminal_at_ms
        && left.resulting_cycle_revision == right.resulting_cycle_revision
        && left.terminal_reason == right.terminal_reason;
}

bool same_identity(const LocalGooseCycleStimulus& left,
                   const LocalGooseCycleStimulus& right) noexcept
{
    return left.scope == right.scope
        && left.id == right.id
        && left.source_kind == right.source_kind
        && left.source_id == right.source_id
        && left.accepted_at_ms == right.accepted_at_ms
        && left.target_cycle_revision == right.target_cycle_revision
        && left.target_cycle_generation == right.target_cycle_generation;
}

void bind_text(sqlite3* database, sqlite3_stmt* statement,
               const int index, const std::string& value)
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

std::optional<std::int64_t> optional_int64(
    sqlite3_stmt* statement, const int column)
{
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return sqlite3_column_int64(statement, column);
}

LocalGooseCycleStimulus read_stimulus(sqlite3_stmt* statement)
{
    const auto accepted_at_ms = sqlite3_column_int64(statement, 4);
    const auto target_revision = sqlite3_column_int64(statement, 5);
    const auto target_generation = sqlite3_column_int64(statement, 6);
    const int raw_status = sqlite3_column_int(statement, 7);

    if (accepted_at_ms < 0 || target_revision < 0 || target_generation < 0
        || raw_status < 0 || raw_status > 3) {
        throw std::runtime_error("invalid Local Goose cycle stimulus row");
    }

    LocalGooseCycleStimulus stimulus;
    stimulus.scope = text(statement, 0);
    stimulus.id = text(statement, 1);
    stimulus.source_kind = text(statement, 2);
    stimulus.source_id = text(statement, 3);
    stimulus.accepted_at_ms = accepted_at_ms;
    stimulus.target_cycle_revision =
        static_cast<std::uint64_t>(target_revision);
    stimulus.target_cycle_generation =
        static_cast<std::uint64_t>(target_generation);
    stimulus.status = static_cast<LocalGooseCycleStimulusStatus>(raw_status);
    stimulus.terminal_at_ms = optional_int64(statement, 8);

    if (sqlite3_column_type(statement, 9) != SQLITE_NULL) {
        const auto resulting_revision = sqlite3_column_int64(statement, 9);
        if (resulting_revision < 0) {
            throw std::runtime_error(
                "invalid Local Goose cycle stimulus resulting revision");
        }
        stimulus.resulting_cycle_revision =
            static_cast<std::uint64_t>(resulting_revision);
    }
    stimulus.terminal_reason = text(statement, 10);

    if (!valid_local_goose_cycle_stimulus(stimulus)) {
        throw std::runtime_error("non-canonical Local Goose cycle stimulus row");
    }
    return stimulus;
}

constexpr const char* stimulus_columns =
    "scope,id,source_kind,source_id,accepted_at_ms,target_cycle_revision,"
    "target_cycle_generation,status,terminal_at_ms,resulting_cycle_revision,"
    "terminal_reason";

std::optional<LocalGooseCycleStimulus> find_by_text(
    sqlite3* database, const char* column, const std::string& value)
{
    const std::string sql =
        std::string{"SELECT "} + stimulus_columns
        + " FROM local_goose_cycle_stimuli WHERE " + column + "=?1";
    Statement statement(database, sql.c_str());
    bind_text(database, statement.get(), 1, value);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto stimulus = read_stimulus(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "ambiguous Local Goose cycle stimulus identity");
    }
    return stimulus;
}

std::optional<LocalGooseCycleStimulus> find_accepted_target(
    sqlite3* database, const std::uint64_t target_cycle_revision)
{
    if (target_cycle_revision
        > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    const std::string sql =
        std::string{"SELECT "} + stimulus_columns
        + " FROM local_goose_cycle_stimuli "
          "WHERE target_cycle_revision=?1 AND status=0";
    Statement statement(database, sql.c_str());
    if (sqlite3_bind_int64(
            statement.get(), 1,
            static_cast<sqlite3_int64>(target_cycle_revision)) != SQLITE_OK) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_DONE) return std::nullopt;
    if (result != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(database));
    auto stimulus = read_stimulus(statement.get());
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        throw std::runtime_error(
            "multiple accepted Local Goose cycle stimuli target one cursor");
    }
    return stimulus;
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
            "cannot stat Local Goose cycle stimulus sidecar");
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid()
        || (status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "Local Goose cycle stimulus sidecar must be a current-user "
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
            "cannot securely create Local Goose cycle stimulus sidecar");
    }
    verify_owner_mode(path);
}

void verify_schema(sqlite3* database)
{
    Statement version_statement(database, "PRAGMA user_version");
    if (sqlite3_step(version_statement.get()) != SQLITE_ROW) {
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    if (sqlite3_column_int(version_statement.get(), 0)
        != local_goose_cycle_stimulus_sidecar_schema) {
        throw std::runtime_error(
            "unsupported Local Goose cycle stimulus sidecar schema");
    }
    if (user_table_count(database) != 1) {
        throw std::runtime_error(
            "Local Goose cycle stimulus sidecar table set differs");
    }

    Statement table_statement(
        database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='local_goose_cycle_stimuli'");
    if (sqlite3_step(table_statement.get()) != SQLITE_ROW
        || sqlite3_column_int64(table_statement.get(), 0) != 1) {
        throw std::runtime_error(
            "Local Goose cycle stimulus table is missing");
    }

    Statement shape_statement(
        database,
        "SELECT scope,id,source_kind,source_id,accepted_at_ms,"
        "target_cycle_revision,target_cycle_generation,status,terminal_at_ms,"
        "resulting_cycle_revision,terminal_reason "
        "FROM local_goose_cycle_stimuli LIMIT 0");
    static_cast<void>(shape_statement);
}

} // namespace

LocalGooseCycleStimulus make_explicit_local_recheck_stimulus(
    const std::string& source_id,
    const std::int64_t accepted_at_ms,
    const std::uint64_t target_cycle_revision,
    const std::uint64_t target_cycle_generation)
{
    LocalGooseCycleStimulus stimulus;
    stimulus.source_id = source_id;
    stimulus.accepted_at_ms = accepted_at_ms;
    stimulus.target_cycle_revision = target_cycle_revision;
    stimulus.target_cycle_generation = target_cycle_generation;
    stimulus.id = std::string{local_goose_cycle_stimulus_id_prefix}
        + sha256_hex(identity_material(
            source_id, target_cycle_revision, target_cycle_generation));

    if (!valid_local_goose_cycle_stimulus(stimulus)) {
        throw std::invalid_argument(
            "explicit Local Goose cycle recheck stimulus is invalid");
    }
    return stimulus;
}

bool valid_local_goose_cycle_stimulus(
    const LocalGooseCycleStimulus& stimulus) noexcept
{
    if (stimulus.scope != local_goose_cycle_stimulus_scope
        || stimulus.source_kind
            != local_goose_cycle_explicit_recheck_source
        || !safe_source_id(stimulus.source_id)
        || stimulus.accepted_at_ms < 0
        || stimulus.target_cycle_revision
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || stimulus.target_cycle_generation < 2
        || stimulus.target_cycle_generation
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || !valid_stimulus_id(stimulus.id)) {
        return false;
    }

    const auto expected_id =
        std::string{local_goose_cycle_stimulus_id_prefix}
        + sha256_hex(identity_material(
            stimulus.source_id,
            stimulus.target_cycle_revision,
            stimulus.target_cycle_generation));
    if (stimulus.id != expected_id) return false;

    switch (stimulus.status) {
    case LocalGooseCycleStimulusStatus::accepted:
        return !stimulus.terminal_at_ms
            && !stimulus.resulting_cycle_revision
            && stimulus.terminal_reason.empty();

    case LocalGooseCycleStimulusStatus::consumed:
        return stimulus.terminal_at_ms
            && *stimulus.terminal_at_ms >= stimulus.accepted_at_ms
            && stimulus.resulting_cycle_revision
            && stimulus.target_cycle_revision
                < static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())
            && *stimulus.resulting_cycle_revision
                == stimulus.target_cycle_revision + 1
            && stimulus.terminal_reason.empty();

    case LocalGooseCycleStimulusStatus::superseded:
    case LocalGooseCycleStimulusStatus::manual_review:
        return stimulus.terminal_at_ms
            && *stimulus.terminal_at_ms >= stimulus.accepted_at_ms
            && !stimulus.resulting_cycle_revision
            && safe_reason(stimulus.terminal_reason);
    }
    return false;
}

bool valid_local_goose_cycle_stimulus_transition(
    const LocalGooseCycleStimulus& expected,
    const LocalGooseCycleStimulus& replacement) noexcept
{
    if (!valid_local_goose_cycle_stimulus(expected)
        || !valid_local_goose_cycle_stimulus(replacement)
        || expected.status != LocalGooseCycleStimulusStatus::accepted
        || replacement.status == LocalGooseCycleStimulusStatus::accepted
        || !same_identity(expected, replacement)) {
        return false;
    }
    return true;
}

LocalGooseCycleStimulusStore::LocalGooseCycleStimulusStore(
    const std::string& path)
{
    ensure_secure_file(path);
    if (sqlite3_open_v2(
            path.c_str(), &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        const std::string message = database_
            ? sqlite3_errmsg(database_)
            : "cannot open Local Goose cycle stimulus sidecar SQLite";
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
        if (version > local_goose_cycle_stimulus_sidecar_schema) {
            throw std::runtime_error(
                "unsupported Local Goose cycle stimulus sidecar schema");
        }

        if (version == 0) {
            if (user_table_count(database_) != 0) {
                throw std::runtime_error(
                    "unversioned Local Goose cycle stimulus sidecar is not empty");
            }
            execute(
                database_,
                "CREATE TABLE local_goose_cycle_stimuli ("
                " scope TEXT NOT NULL,"
                " id TEXT PRIMARY KEY NOT NULL,"
                " source_kind TEXT NOT NULL,"
                " source_id TEXT NOT NULL,"
                " accepted_at_ms INTEGER NOT NULL CHECK(accepted_at_ms >= 0),"
                " target_cycle_revision INTEGER NOT NULL "
                " CHECK(target_cycle_revision >= 0),"
                " target_cycle_generation INTEGER NOT NULL "
                " CHECK(target_cycle_generation >= 2),"
                " status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 3),"
                " terminal_at_ms INTEGER "
                " CHECK(terminal_at_ms IS NULL OR terminal_at_ms >= 0),"
                " resulting_cycle_revision INTEGER "
                " CHECK(resulting_cycle_revision IS NULL "
                " OR resulting_cycle_revision >= 0),"
                " terminal_reason TEXT NOT NULL,"
                " UNIQUE(scope,source_kind,source_id)"
                ");");
            execute(
                database_,
                "CREATE UNIQUE INDEX "
                "local_goose_cycle_stimuli_one_accepted_target "
                "ON local_goose_cycle_stimuli(target_cycle_revision) "
                "WHERE status=0;");
            execute(
                database_,
                "PRAGMA user_version=1;");
        }

        verify_schema(database_);
        execute(database_, "COMMIT;");
    } catch (...) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LocalGooseCycleStimulusStore::~LocalGooseCycleStimulusStore()
{
    sqlite3_close(database_);
}

std::optional<LocalGooseCycleStimulus>
LocalGooseCycleStimulusStore::find(const std::string& id) const
{
    return find_by_text(database_, "id", id);
}

std::optional<LocalGooseCycleStimulus>
LocalGooseCycleStimulusStore::find_by_source(const std::string& source_id) const
{
    return find_by_text(database_, "source_id", source_id);
}

std::optional<LocalGooseCycleStimulus>
LocalGooseCycleStimulusStore::find_accepted_for_target(
    const std::uint64_t target_cycle_revision) const
{
    return find_accepted_target(database_, target_cycle_revision);
}

LocalGooseCycleStimulusStoreWrite LocalGooseCycleStimulusStore::append(
    const LocalGooseCycleStimulus& stimulus)
{
    if (!valid_local_goose_cycle_stimulus(stimulus)
        || stimulus.status != LocalGooseCycleStimulusStatus::accepted) {
        return {LocalGooseCycleStimulusStoreResult::invalid, {},
                "stimulus append requires canonical accepted record"};
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");

        if (const auto existing = find_by_text(
                database_, "id", stimulus.id)) {
            execute(database_, "ROLLBACK;");
            return {same_stimulus(*existing, stimulus)
                        ? LocalGooseCycleStimulusStoreResult::duplicate
                        : LocalGooseCycleStimulusStoreResult::conflict,
                    existing,
                    same_stimulus(*existing, stimulus)
                        ? std::string{}
                        : "stimulus id conflicts with durable state"};
        }
        if (const auto existing = find_by_text(
                database_, "source_id", stimulus.source_id)) {
            execute(database_, "ROLLBACK;");
            return {LocalGooseCycleStimulusStoreResult::conflict, existing,
                    "stimulus source identity was already used"};
        }
        if (const auto existing = find_accepted_target(
                database_, stimulus.target_cycle_revision)) {
            execute(database_, "ROLLBACK;");
            return {LocalGooseCycleStimulusStoreResult::conflict, existing,
                    "target cursor revision already has an accepted stimulus"};
        }

        Statement statement(
            database_,
            "INSERT INTO local_goose_cycle_stimuli("
            "scope,id,source_kind,source_id,accepted_at_ms,"
            "target_cycle_revision,target_cycle_generation,status,"
            "terminal_at_ms,resulting_cycle_revision,terminal_reason"
            ") VALUES(?1,?2,?3,?4,?5,?6,?7,?8,NULL,NULL,'')");
        bind_text(database_, statement.get(), 1, stimulus.scope);
        bind_text(database_, statement.get(), 2, stimulus.id);
        bind_text(database_, statement.get(), 3, stimulus.source_kind);
        bind_text(database_, statement.get(), 4, stimulus.source_id);
        if (sqlite3_bind_int64(
                statement.get(), 5, stimulus.accepted_at_ms) != SQLITE_OK
            || sqlite3_bind_int64(
                statement.get(), 6,
                static_cast<sqlite3_int64>(
                    stimulus.target_cycle_revision)) != SQLITE_OK
            || sqlite3_bind_int64(
                statement.get(), 7,
                static_cast<sqlite3_int64>(
                    stimulus.target_cycle_generation)) != SQLITE_OK
            || sqlite3_bind_int(
                statement.get(), 8,
                static_cast<int>(stimulus.status)) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        if (sqlite3_step(statement.get()) != SQLITE_DONE) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        execute(database_, "COMMIT;");
        return {LocalGooseCycleStimulusStoreResult::accepted, stimulus, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalGooseCycleStimulusStoreResult::unavailable, {},
                error.what()};
    }
}

LocalGooseCycleStimulusStoreWrite LocalGooseCycleStimulusStore::replace(
    const LocalGooseCycleStimulus& expected,
    const LocalGooseCycleStimulus& replacement)
{
    if (!valid_local_goose_cycle_stimulus_transition(
            expected, replacement)) {
        return {LocalGooseCycleStimulusStoreResult::invalid, {},
                "stimulus replacement transition is non-canonical"};
    }

    try {
        execute(database_, "BEGIN IMMEDIATE;");
        const auto current = find_by_text(database_, "id", expected.id);
        if (!current) {
            execute(database_, "ROLLBACK;");
            return {LocalGooseCycleStimulusStoreResult::conflict, {},
                    "stimulus disappeared before transition"};
        }
        if (same_stimulus(*current, replacement)) {
            execute(database_, "ROLLBACK;");
            return {LocalGooseCycleStimulusStoreResult::duplicate,
                    current, {}};
        }
        if (!same_stimulus(*current, expected)) {
            execute(database_, "ROLLBACK;");
            return {LocalGooseCycleStimulusStoreResult::conflict,
                    current,
                    "stimulus changed before compare-and-swap"};
        }

        Statement statement(
            database_,
            "UPDATE local_goose_cycle_stimuli SET "
            "status=?1,terminal_at_ms=?2,resulting_cycle_revision=?3,"
            "terminal_reason=?4 WHERE id=?5 AND status=0");
        if (sqlite3_bind_int(
                statement.get(), 1,
                static_cast<int>(replacement.status)) != SQLITE_OK
            || sqlite3_bind_int64(
                statement.get(), 2,
                *replacement.terminal_at_ms) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        if (replacement.resulting_cycle_revision) {
            if (sqlite3_bind_int64(
                    statement.get(), 3,
                    static_cast<sqlite3_int64>(
                        *replacement.resulting_cycle_revision)) != SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(database_));
            }
        } else if (sqlite3_bind_null(statement.get(), 3) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(database_));
        }
        bind_text(
            database_, statement.get(), 4, replacement.terminal_reason);
        bind_text(database_, statement.get(), 5, replacement.id);

        if (sqlite3_step(statement.get()) != SQLITE_DONE
            || sqlite3_changes(database_) != 1) {
            throw std::runtime_error(
                "stimulus compare-and-swap update did not affect one row");
        }
        execute(database_, "COMMIT;");
        return {LocalGooseCycleStimulusStoreResult::accepted,
                replacement, {}};
    } catch (const std::exception& error) {
        sqlite3_exec(database_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return {LocalGooseCycleStimulusStoreResult::unavailable, {},
                error.what()};
    }
}

LocalGooseCycleStimulusSidecarInspection
inspect_local_goose_cycle_stimulus_sidecar(
    const std::string& path) noexcept
{
    LocalGooseCycleStimulusSidecarInspection out;
    sqlite3* database = nullptr;

    try {
        verify_owner_mode(path);
        if (sqlite3_open_v2(
                path.c_str(), &database,
                SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                nullptr) != SQLITE_OK) {
            throw std::runtime_error(
                database ? sqlite3_errmsg(database)
                         : "cannot open Local Goose cycle stimulus sidecar read-only");
        }
        execute(database, "PRAGMA query_only=ON;");
        execute(database, "PRAGMA busy_timeout=5000;");
        verify_schema(database);

        const std::string sql =
            std::string{"SELECT "} + stimulus_columns
            + " FROM local_goose_cycle_stimuli ORDER BY accepted_at_ms,id";
        Statement statement(database, sql.c_str());
        while (true) {
            const int result = sqlite3_step(statement.get());
            if (result == SQLITE_DONE) break;
            if (result != SQLITE_ROW) {
                throw std::runtime_error(sqlite3_errmsg(database));
            }
            if (out.stimuli.size() >= max_inspection_records) {
                throw std::runtime_error(
                    "Local Goose cycle stimulus inspection record bound exceeded");
            }
            out.stimuli.push_back(read_stimulus(statement.get()));
        }
        sqlite3_close(database);
        database = nullptr;
        out.eligible = true;
        return out;
    } catch (const std::exception& error) {
        sqlite3_close(database);
        out.detail = error.what();
        return out;
    } catch (...) {
        sqlite3_close(database);
        out.detail = "Local Goose cycle stimulus sidecar inspection failed";
        return out;
    }
}

} // namespace gaudere_agent
