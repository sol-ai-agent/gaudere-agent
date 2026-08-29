#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using PulseResult = gaudere_agent::AutonomousCognitionPulseResult;
using PulseState = gaudere_agent::AutonomousCognitionPulseState;

struct Options {
    std::string state_path;
    std::string sidecar_path;
    std::string predecessor_task_id;
    bool check = false;
    bool seed = false;
    bool observe = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program << " --state PATH --sidecar PATH "
        << "[--check | --seed-after-explicit-autonomous-pulse-go TASK_ID | "
        << "--observe-after-explicit-autonomous-pulse-go]\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--sidecar" && index + 1 < argc) {
            options.sidecar_path = argv[++index];
        } else if (argument == "--check") {
            options.check = true;
        } else if (argument == "--seed-after-explicit-autonomous-pulse-go"
                   && index + 1 < argc) {
            options.seed = true;
            options.predecessor_task_id = argv[++index];
        } else if (argument == "--observe-after-explicit-autonomous-pulse-go") {
            options.observe = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }

    if (options.state_path.empty())
        throw std::invalid_argument("--state PATH is required");
    if (options.sidecar_path.empty())
        throw std::invalid_argument("--sidecar PATH is required");
    const int modes = static_cast<int>(options.check)
        + static_cast<int>(options.seed) + static_cast<int>(options.observe);
    if (modes != 1)
        throw std::invalid_argument("exactly one pulse mode is required");
    if (options.seed && options.predecessor_task_id.empty())
        throw std::invalid_argument("pulse predecessor Task ID must not be empty");
    return options;
}

const char* result_name(const PulseResult result) noexcept
{
    switch (result) {
    case PulseResult::disabled: return "disabled";
    case PulseResult::unseeded: return "unseeded";
    case PulseResult::seeded: return "seeded";
    case PulseResult::duplicate: return "duplicate";
    case PulseResult::not_due: return "not_due";
    case PulseResult::budget_blocked: return "budget_blocked";
    case PulseResult::preparing: return "preparing";
    case PulseResult::prepared: return "prepared";
    case PulseResult::waiting: return "waiting";
    case PulseResult::settled_continue: return "settled_continue";
    case PulseResult::settled_stop: return "settled_stop";
    case PulseResult::clock_rollback: return "clock_rollback";
    case PulseResult::blocked: return "blocked";
    case PulseResult::conflict: return "conflict";
    case PulseResult::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* state_name(const PulseState state) noexcept
{
    switch (state) {
    case PulseState::idle: return "idle";
    case PulseState::preparing: return "preparing";
    case PulseState::prepared: return "prepared";
    case PulseState::blocked: return "blocked";
    case PulseState::quiescent: return "quiescent";
    }
    return "unknown";
}

class ReadOnlyDatabase {
public:
    explicit ReadOnlyDatabase(const std::string& path)
    {
        if (sqlite3_open_v2(path.c_str(), &database_,
                            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = database_ ? sqlite3_errmsg(database_)
                                                   : "cannot open pulse sidecar";
            sqlite3_close(database_);
            database_ = nullptr;
            throw std::runtime_error(message);
        }
    }

    ~ReadOnlyDatabase() { sqlite3_close(database_); }
    sqlite3* get() const noexcept { return database_; }

private:
    sqlite3* database_ = nullptr;
};

int scalar_int(sqlite3* database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database));
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(database));
    }
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return value;
}

std::string column_text(sqlite3_stmt* statement, const int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    const int bytes = sqlite3_column_bytes(statement, column);
    if (!value || bytes <= 0) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(bytes));
}

int check_paths(const Options& options)
{
    std::error_code error;
    const bool state_exists = std::filesystem::is_regular_file(
        std::filesystem::symlink_status(options.state_path, error));
    if (error) throw std::runtime_error("cannot inspect main state path");

    error.clear();
    const auto sidecar_status = std::filesystem::symlink_status(
        options.sidecar_path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("cannot inspect pulse sidecar path");
    const bool sidecar_exists = !error
        && std::filesystem::is_regular_file(sidecar_status);

    std::cout << "mode=check\n"
              << "state_exists=" << (state_exists ? "true" : "false") << '\n'
              << "sidecar_exists=" << (sidecar_exists ? "true" : "false") << '\n';

    if (!sidecar_exists) {
        std::cout << "pulse_seeded=false\nprovider_effects=0\n";
        return state_exists ? 0 : 2;
    }

    ReadOnlyDatabase database(options.sidecar_path);
    const int schema = scalar_int(database.get(), "PRAGMA user_version");
    if (schema != gaudere_agent::autonomous_cognition_pulse_sidecar_schema) {
        std::cout << "sidecar_schema=" << schema
                  << "\npulse_seeded=unknown\nprovider_effects=0\n";
        return 3;
    }

    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "SELECT generation,state,predecessor_task_id,anchor_at_ms,due_at_ms,"
        "observed_at_ms,snapshot_task_id,current_task_id,blocked_reason "
        "FROM autonomous_cognition_pulse_cursor WHERE scope=?1";
    if (sqlite3_prepare_v2(database.get(), sql, -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    if (sqlite3_bind_text(statement, 1,
            gaudere_agent::autonomous_cognition_pulse_scope, -1, SQLITE_STATIC)
        != SQLITE_OK) {
        sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }

    const int first = sqlite3_step(statement);
    if (first == SQLITE_DONE) {
        sqlite3_finalize(statement);
        std::cout << "sidecar_schema=1\npulse_seeded=false\nprovider_effects=0\n";
        return state_exists ? 0 : 2;
    }
    if (first != SQLITE_ROW) {
        sqlite3_finalize(statement);
        throw std::runtime_error(sqlite3_errmsg(database.get()));
    }

    const auto generation = sqlite3_column_int64(statement, 0);
    const auto state_value = sqlite3_column_int(statement, 1);
    const auto predecessor = column_text(statement, 2);
    const auto anchor = sqlite3_column_int64(statement, 3);
    const auto due = sqlite3_column_int64(statement, 4);
    const bool observed = sqlite3_column_type(statement, 5) != SQLITE_NULL;
    const auto observed_at = observed ? sqlite3_column_int64(statement, 5) : -1;
    const auto snapshot = column_text(statement, 6);
    const auto current = column_text(statement, 7);
    const auto blocked = column_text(statement, 8);
    const int second = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (second != SQLITE_DONE || state_value < 0 || state_value > 4) {
        std::cout << "sidecar_schema=1\npulse_seeded=invalid\nprovider_effects=0\n";
        return 3;
    }

    std::cout << "sidecar_schema=1\npulse_seeded=true\n"
              << "generation=" << generation << '\n'
              << "state=" << state_name(static_cast<PulseState>(state_value)) << '\n'
              << "predecessor_task_id=" << predecessor << '\n'
              << "anchor_at_ms=" << anchor << '\n'
              << "due_at_ms=" << due << '\n'
              << "observed_at_ms=";
    if (observed) std::cout << observed_at;
    else std::cout << "null";
    std::cout << '\n'
              << "snapshot_task_id=" << snapshot << '\n'
              << "current_task_id=" << current << '\n'
              << "blocked_reason=" << blocked << '\n'
              << "provider_effects=0\n";
    return state_exists ? 0 : 2;
}

void print_observation(
    const gaudere_agent::AutonomousCognitionPulseObservation& observation)
{
    std::cout << "result=" << result_name(observation.result) << '\n';
    if (!observation.detail.empty())
        std::cout << "detail=" << observation.detail << '\n';
    if (observation.cursor) {
        const auto& cursor = *observation.cursor;
        std::cout << "generation=" << cursor.generation << '\n'
                  << "state=" << state_name(cursor.state) << '\n'
                  << "predecessor_task_id=" << cursor.predecessor_task_id << '\n'
                  << "anchor_at_ms=" << cursor.anchor_at_ms << '\n'
                  << "due_at_ms=" << cursor.due_at_ms << '\n'
                  << "snapshot_task_id=" << cursor.snapshot_task_id << '\n'
                  << "current_task_id=" << cursor.current_task_id << '\n';
    }
    if (observation.task) {
        std::cout << "task_id=" << observation.task->id << '\n'
                  << "task_kind=" << observation.task->kind << '\n';
    }
    std::cout << "provider_effects=0\n";
}

int mutation_exit_code(const PulseResult result) noexcept
{
    switch (result) {
    case PulseResult::disabled:
    case PulseResult::unseeded:
        return 2;
    case PulseResult::clock_rollback:
    case PulseResult::blocked:
    case PulseResult::conflict:
    case PulseResult::unavailable:
        return 3;
    default:
        return 0;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;

        if (options.check) return check_paths(options);

        gaudere_agent::StateLock state_lock(options.state_path);
        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);
        gaudere::persistence::sqlite::WakeIntentStore wake_store(options.state_path);
        gaudere::work::Runtime work_runtime(task_store, now);
        work_runtime.recover();
        gaudere_agent::AutonomousCognitionPulseStore pulse_store(options.sidecar_path);
        gaudere_agent::AutonomousCognitionPulse pulse(
            pulse_store, task_store, budget_store, wake_store, work_runtime, now, true);

        const auto observation = options.seed
            ? pulse.seed(options.predecessor_task_id)
            : pulse.observe();
        print_observation(observation);
        return mutation_exit_code(observation.result);
    } catch (const std::exception& error) {
        std::cerr << "gaudere-autonomous-cognition-pulse: " << error.what() << '\n';
        return 1;
    }
}
