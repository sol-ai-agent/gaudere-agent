#include "ContinuityDeltaCheckpoint.hpp"
#include "OpenAIBudget.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string state_path;
    std::string audited_task_id;
    bool execute = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " --state PATH --audited-task-id ID"
        << " --checkpoint-after-explicit-go\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--audited-task-id" && index + 1 < argc) {
            options.audited_task_id = argv[++index];
        } else if (argument == "--checkpoint-after-explicit-go") {
            options.execute = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    if (options.state_path.empty()) {
        throw std::invalid_argument("--state PATH is required");
    }
    if (options.audited_task_id.empty()) {
        throw std::invalid_argument("--audited-task-id ID is required");
    }
    if (!options.execute) {
        throw std::invalid_argument("--checkpoint-after-explicit-go is required");
    }
    return options;
}

const char* result_name(const gaudere_agent::ContinuityDeltaCheckpointResult result)
{
    using Result = gaudere_agent::ContinuityDeltaCheckpointResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::ineligible: return "ineligible";
    case Result::conflict: return "conflict";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
}

bool success(const gaudere_agent::ContinuityDeltaCheckpointResult result)
{
    using Result = gaudere_agent::ContinuityDeltaCheckpointResult;
    return result == Result::accepted || result == Result::duplicate;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;

        // All durable access follows explicit CLI authority and exclusive state
        // ownership. This one-shot has no provider, secret or network object.
        gaudere_agent::StateLock state_lock(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);
        gaudere::persistence::sqlite::WakeIntentStore wake_store(options.state_path);

        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::work::Runtime work_runtime(task_store, now);
        work_runtime.recover();

        const auto policy = gaudere_agent::openai_bootstrap_budget_policy();
        const auto before = budget_store.snapshot(
            std::string{gaudere_agent::openai_budget_scope()}, now(), policy);

        gaudere_agent::ContinuityDeltaCheckpoint checkpoint(
            task_store, action_store, budget_store, wake_store, work_runtime, now,
            [](const std::string_view phase) {
                std::cout << "phase=" << phase << '\n';
            });
        const auto record = checkpoint.checkpoint(options.audited_task_id);

        const auto after = budget_store.snapshot(
            std::string{gaudere_agent::openai_budget_scope()}, now(), policy);
        std::cout << "result=" << result_name(record.result) << '\n'
                  << "audited_task_id=" << options.audited_task_id << '\n'
                  << "provider_total_before=" << before.total_used << '\n'
                  << "provider_total_after=" << after.total_used << '\n'
                  << "provider_effects="
                  << (before.total_used == after.total_used ? 0 : 1) << '\n';
        if (!record.detail.empty()) std::cout << "detail=" << record.detail << '\n';
        if (record.task) {
            std::cout << "checkpoint_task_id=" << record.task->id << '\n'
                      << "checkpoint_terminal="
                      << (gaudere::work::is_terminal(record.task->status)
                              ? "true" : "false")
                      << '\n';
            if (record.task->result) {
                std::cout << "result_content_type="
                          << record.task->result->content_type << '\n';
            }
        }

        if (before.total_used != after.total_used) {
            throw std::runtime_error(
                "provider budget changed during provider-free checkpoint");
        }
        return success(record.result) ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-continuity-delta-checkpoint: "
                  << error.what() << '\n';
        return 1;
    }
}
