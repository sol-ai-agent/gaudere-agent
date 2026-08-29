#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "OpenAIBudget.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::size_t max_task_id_bytes = 1024;

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
        << "Usage: " << program << " --state PATH --sidecar PATH --check\n"
        << "       " << program
        << " --state PATH --sidecar PATH --predecessor-task-id ID"
        << " --seed-after-explicit-pulse-go\n"
        << "       " << program
        << " --state PATH --sidecar PATH --observe-after-explicit-pulse-go\n";
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
        } else if (argument == "--predecessor-task-id" && index + 1 < argc) {
            options.predecessor_task_id = argv[++index];
        } else if (argument == "--check") {
            options.check = true;
        } else if (argument == "--seed-after-explicit-pulse-go") {
            options.seed = true;
        } else if (argument == "--observe-after-explicit-pulse-go") {
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
    if (std::filesystem::path(options.state_path) == std::filesystem::path(options.sidecar_path))
        throw std::invalid_argument("--state and --sidecar must be different paths");

    const int modes = static_cast<int>(options.check)
        + static_cast<int>(options.seed) + static_cast<int>(options.observe);
    if (modes != 1)
        throw std::invalid_argument("choose exactly one pulse mode");

    if (options.seed) {
        if (options.predecessor_task_id.empty()
            || options.predecessor_task_id.size() > max_task_id_bytes) {
            throw std::invalid_argument(
                "seed requires --predecessor-task-id ID of 1..1024 bytes");
        }
    } else if (!options.predecessor_task_id.empty()) {
        throw std::invalid_argument(
            "--predecessor-task-id is accepted only in seed mode");
    }
    return options;
}

const char* result_name(const gaudere_agent::AutonomousCognitionPulseResult result) noexcept
{
    using Result = gaudere_agent::AutonomousCognitionPulseResult;
    switch (result) {
    case Result::disabled: return "disabled";
    case Result::unseeded: return "unseeded";
    case Result::seeded: return "seeded";
    case Result::duplicate: return "duplicate";
    case Result::not_due: return "not_due";
    case Result::budget_blocked: return "budget_blocked";
    case Result::preparing: return "preparing";
    case Result::prepared: return "prepared";
    case Result::waiting: return "waiting";
    case Result::settled_continue: return "settled_continue";
    case Result::settled_stop: return "settled_stop";
    case Result::clock_rollback: return "clock_rollback";
    case Result::blocked: return "blocked";
    case Result::conflict: return "conflict";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* state_name(const gaudere_agent::AutonomousCognitionPulseState state) noexcept
{
    using State = gaudere_agent::AutonomousCognitionPulseState;
    switch (state) {
    case State::idle: return "idle";
    case State::preparing: return "preparing";
    case State::prepared: return "prepared";
    case State::blocked: return "blocked";
    case State::quiescent: return "quiescent";
    }
    return "unknown";
}

void print_cursor(const gaudere_agent::AutonomousCognitionPulseCursor& cursor)
{
    std::cout << "pulse_scope=" << cursor.scope << '\n'
              << "pulse_revision=" << cursor.revision << '\n'
              << "pulse_generation=" << cursor.generation << '\n'
              << "pulse_state=" << state_name(cursor.state) << '\n'
              << "predecessor_task_id=" << cursor.predecessor_task_id << '\n'
              << "anchor_at_ms=" << cursor.anchor_at_ms << '\n'
              << "due_at_ms=" << cursor.due_at_ms << '\n';
    if (cursor.observed_at_ms)
        std::cout << "observed_at_ms=" << *cursor.observed_at_ms << '\n';
    if (!cursor.snapshot_task_id.empty())
        std::cout << "snapshot_task_id=" << cursor.snapshot_task_id << '\n';
    if (!cursor.current_task_id.empty())
        std::cout << "current_task_id=" << cursor.current_task_id << '\n';
    if (!cursor.blocked_reason.empty())
        std::cout << "blocked_reason=" << cursor.blocked_reason << '\n';
}

int check_sidecar(const Options& options)
{
    if (!std::filesystem::exists(options.state_path))
        throw std::runtime_error("state database does not exist");

    // Important: an absent sidecar is an observable unseeded state. Do not construct
    // AutonomousCognitionPulseStore here because its SQLite open is allowed to create.
    if (!std::filesystem::exists(options.sidecar_path)) {
        std::cout << "mode=check\n"
                  << "pulse_state=unseeded\n"
                  << "sidecar_exists=false\n"
                  << "provider_effects=0\n";
        return 0;
    }
    if (!std::filesystem::is_regular_file(options.sidecar_path))
        throw std::runtime_error("pulse sidecar must be a regular file");

    gaudere_agent::AutonomousCognitionPulseStore pulse_store(options.sidecar_path);
    const auto cursor = pulse_store.find(gaudere_agent::autonomous_cognition_pulse_scope);
    std::cout << "mode=check\nsidecar_exists=true\nprovider_effects=0\n";
    if (!cursor) {
        std::cout << "pulse_state=unseeded\n";
        return 0;
    }
    print_cursor(*cursor);
    return cursor->state == gaudere_agent::AutonomousCognitionPulseState::blocked ? 2 : 0;
}

int mutate(const Options& options)
{
    if (!std::filesystem::exists(options.state_path))
        throw std::runtime_error("state database does not exist");

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

    std::cout << "mode=" << (options.seed ? "seed" : "observe") << '\n'
              << "pulse_result=" << result_name(observation.result) << '\n'
              << "provider_effects=0\n";
    if (observation.cursor) print_cursor(*observation.cursor);
    if (observation.task) std::cout << "task_id=" << observation.task->id << '\n';
    if (!observation.detail.empty())
        std::cout << "detail=" << observation.detail << '\n';

    using Result = gaudere_agent::AutonomousCognitionPulseResult;
    switch (observation.result) {
    case Result::seeded:
    case Result::duplicate:
    case Result::not_due:
    case Result::preparing:
    case Result::prepared:
    case Result::waiting:
    case Result::settled_continue:
    case Result::settled_stop:
    case Result::budget_blocked:
        return 0;
    case Result::disabled:
    case Result::unseeded:
    case Result::clock_rollback:
    case Result::blocked:
    case Result::conflict:
    case Result::unavailable:
        return 3;
    }
    return 3;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;
        return options.check ? check_sidecar(options) : mutate(options);
    } catch (const std::exception& error) {
        std::cerr << "gaudere-autonomous-cognition-pulse: " << error.what() << '\n';
        return 1;
    }
}
