#include "OpenAIBudget.hpp"
#include "OpenAIStructuredActivation.hpp"
#include "ResumeAfterWakeV1.hpp"
#include "ResumeAfterWakeV1Cognition.hpp"
#include "ResumeAfterWakeV1TextInputAdapter.hpp"
#include "ResumeDecisionStructuredOutput.hpp"
#include "StateLock.hpp"
#include "TaskExecutor.hpp"
#include "WakeSourceDecision.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string state_path;
    bool check = false;
    bool execute = false;
    std::string model;
    std::string secret = "gaudere-openai-api-key";
    std::string secret_directory = "/run/secrets";
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program << " --state PATH --check\n"
        << "       " << program
        << " --state PATH --execute-after-explicit-resume-v1-go"
        << " --openai-model MODEL [--openai-secret NAME]"
        << " [--secret-dir PATH]\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--check") {
            options.check = true;
        } else if (argument == "--execute-after-explicit-resume-v1-go") {
            options.execute = true;
        } else if (argument == "--openai-model" && index + 1 < argc) {
            options.model = argv[++index];
        } else if (argument == "--openai-secret" && index + 1 < argc) {
            options.secret = argv[++index];
        } else if (argument == "--secret-dir" && index + 1 < argc) {
            options.secret_directory = argv[++index];
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
    if (options.check == options.execute) {
        throw std::invalid_argument(
            "choose exactly one of --check or --execute-after-explicit-resume-v1-go");
    }
    if (options.check && (!options.model.empty()
        || options.secret != "gaudere-openai-api-key"
        || options.secret_directory != "/run/secrets")) {
        throw std::invalid_argument(
            "--check does not accept provider configuration");
    }
    if (options.execute && options.model.empty()) {
        throw std::invalid_argument(
            "--execute-after-explicit-resume-v1-go requires --openai-model");
    }
    if (options.secret.empty() || options.secret_directory.empty()) {
        throw std::invalid_argument("provider secret configuration must not be empty");
    }
    return options;
}

std::string sole_wake_id(
    gaudere::scheduling::wake::WakeIntentStore& wake_store)
{
    const auto inspection =
        wake_store.inspect_scope(gaudere_agent::bounded_reflection_wake_scope);
    switch (inspection.result) {
    case gaudere::scheduling::wake::WakeIntentScopeResult::one:
        if (!inspection.intent) {
            throw std::runtime_error(
                "wake scope reported one intent without durable record");
        }
        return inspection.intent->id;
    case gaudere::scheduling::wake::WakeIntentScopeResult::empty:
        throw std::runtime_error("resume wake scope is empty");
    case gaudere::scheduling::wake::WakeIntentScopeResult::ambiguous:
        throw std::runtime_error("resume wake scope is ambiguous");
    }
    throw std::logic_error("unknown wake scope inspection result");
}

const char* budget_name(const gaudere::budget::ConsumeResult result) noexcept
{
    using Result = gaudere::budget::ConsumeResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::total_exhausted: return "total_exhausted";
    case Result::window_exhausted: return "window_exhausted";
    case Result::cooldown: return "cooldown";
    case Result::clock_rollback: return "clock_rollback";
    }
    return "unknown";
}

std::int64_t epoch_milliseconds(const std::chrono::system_clock::time_point value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

bool fresh_for_real_call(const gaudere::work::Task& task,
                         const std::chrono::system_clock::time_point now,
                         std::int64_t& age_ms)
{
    const auto captured =
        gaudere_agent::resume_after_wake_v1_captured_at_ms(task);
    if (!captured) return false;
    const auto now_ms = epoch_milliseconds(now);
    if (now_ms < *captured) return false;
    age_ms = now_ms - *captured;
    const auto max_age = std::chrono::duration_cast<std::chrono::milliseconds>(
        gaudere_agent::resume_after_wake_v1_max_snapshot_age).count();
    return age_ms <= max_age;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;

        gaudere_agent::StateLock state_lock(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::WakeIntentStore wake_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);

        const auto wake_id = sole_wake_id(wake_store);
        const auto task_id =
            std::string{gaudere_agent::resume_after_wake_v1_task_prefix} + wake_id;
        const auto task = task_store.find(task_id);
        if (!task) {
            throw std::runtime_error("durable resume-after-wake v1 Task is missing");
        }
        if (!gaudere_agent::valid_resume_after_wake_v1_task(*task)) {
            throw std::runtime_error("durable resume-after-wake v1 Task is non-canonical");
        }

        const auto now = std::chrono::system_clock::now();
        std::int64_t context_age_ms = -1;
        const bool fresh = fresh_for_real_call(*task, now, context_age_ms);
        const auto policy = gaudere_agent::openai_bootstrap_budget_policy();
        const auto budget = budget_store.snapshot(
            std::string{gaudere_agent::openai_budget_scope()}, now, policy);

        std::cout << "resume_task_id=" << task_id << '\n'
                  << "resume_task_terminal="
                  << (gaudere::work::is_terminal(task->status) ? "true" : "false")
                  << '\n'
                  << "context_age_ms=" << context_age_ms << '\n'
                  << "context_fresh=" << (fresh ? "true" : "false") << '\n'
                  << "provider_total_used=" << budget.total_used << '\n'
                  << "provider_window_used=" << budget.in_window_used << '\n'
                  << "provider_next=" << budget_name(budget.next_new_consumption)
                  << '\n';

        if (options.check) {
            std::cout << "mode=check\nprovider_effects=0\n";
            return fresh
                    && budget.next_new_consumption
                        == gaudere::budget::ConsumeResult::accepted
                ? 0 : 2;
        }

        if (gaudere::work::is_terminal(task->status)) {
            std::cout << "provider_call_started=false\nreason=resume_task_terminal\n";
            return 0;
        }
        if (!fresh) {
            std::cerr << "gaudere-resume-after-wake-v1: frozen context is not fresh enough for a real provider call\n";
            return 3;
        }
        if (budget.next_new_consumption != gaudere::budget::ConsumeResult::accepted) {
            std::cerr << "gaudere-resume-after-wake-v1: provider budget does not admit a new call\n";
            return 4;
        }

        const auto clock = [] { return std::chrono::system_clock::now(); };
        gaudere::work::Runtime work_runtime(task_store, clock);
        work_runtime.recover();

        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::scheduling::wake::Runtime action_runtime(action_store, clock);
        action_runtime.recover();

        // The Structured Output contract is immutable construction-time provider
        // configuration. Budget/Action/no-replay authority remains unchanged in
        // ProviderTaskHandler, and the cognition normalizer still validates the
        // model proposal after the API-level shape guarantee.
        gaudere_agent::OpenAIStructuredActivation activation(
            action_runtime, action_store, budget_store,
            gaudere_agent::resume_decision_structured_output_contract(),
            options.model, options.secret, options.secret_directory);
        gaudere_agent::ResumeAfterWakeV1TextInputAdapter text_input(
            activation.handler());
        gaudere_agent::ResumeAfterWakeV1CognitionHandler cognition(text_input);
        gaudere_agent::TaskExecutor executor(work_runtime, task_store);

        const auto execution = executor.execute(
            task_id, "resume-after-wake-v1-one-shot", cognition);
        if (execution != gaudere_agent::ExecuteResult::completed) {
            std::cerr << "gaudere-resume-after-wake-v1: execution did not complete\n";
            return 5;
        }

        const auto stored = task_store.find(task_id);
        if (!stored || !gaudere::work::is_terminal(stored->status)) {
            throw std::runtime_error(
                "resume-after-wake v1 Task is not terminal after execution");
        }

        const auto after = budget_store.snapshot(
            std::string{gaudere_agent::openai_budget_scope()},
            std::chrono::system_clock::now(), policy);
        std::cout << "provider_total_used_after=" << after.total_used << '\n'
                  << "resume_task_terminal=true\n";
        if (stored->result) {
            std::cout << "result_content_type=" << stored->result->content_type << '\n'
                      << "result_output=" << stored->result->output << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-resume-after-wake-v1: " << error.what() << '\n';
        return 1;
    }
}
