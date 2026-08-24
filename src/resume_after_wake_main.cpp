#include "OpenAIActivation.hpp"
#include "ResumeAfterWake.hpp"
#include "ResumeAfterWakeCognition.hpp"
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
#include <optional>
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
        << " --state PATH --execute-after-explicit-resume-go"
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
        } else if (argument == "--execute-after-explicit-resume-go") {
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
            "choose exactly one of --check or --execute-after-explicit-resume-go");
    }
    if (options.check && (!options.model.empty()
        || options.secret != "gaudere-openai-api-key"
        || options.secret_directory != "/run/secrets")) {
        throw std::invalid_argument(
            "--check does not accept provider configuration");
    }
    if (options.execute && options.model.empty()) {
        throw std::invalid_argument(
            "--execute-after-explicit-resume-go requires --openai-model");
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

const char* claim_name(const gaudere_agent::ResumeAfterWakeClaimResult result)
{
    using Result = gaudere_agent::ResumeAfterWakeClaimResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::disabled: return "disabled";
    case Result::wake_not_found: return "wake_not_found";
    case Result::ineligible: return "ineligible";
    case Result::conflict: return "conflict";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
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
        const auto wake_id = sole_wake_id(wake_store);
        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::work::Runtime work_runtime(task_store, now);

        gaudere_agent::ResumeAfterWake resume(
            task_store, wake_store, work_runtime, true);

        if (options.check) {
            const auto status = resume.inspect(wake_id);
            std::cout << status.report;
            std::cout << "mode=check\nprovider_effects=0\n";
            return status.healthy ? 0 : 2;
        }

        // Execution is deliberately one-shot and requires exclusive state ownership.
        // Runtime recovery is performed only after the explicit execution token was
        // supplied and the StateLock has proved no service process owns this DB.
        work_runtime.recover();
        const auto claim = resume.claim(wake_id);
        std::cout << "claim=" << claim_name(claim.result) << '\n';
        if (claim.result != gaudere_agent::ResumeAfterWakeClaimResult::accepted
            && claim.result != gaudere_agent::ResumeAfterWakeClaimResult::duplicate) {
            std::cerr << "gaudere-resume-after-wake: claim failed: "
                      << claim.detail << '\n';
            return 3;
        }
        if (!claim.task || claim.task->kind != gaudere_agent::resume_after_wake_task_kind) {
            throw std::runtime_error(
                "canonical resume Task is missing after successful claim");
        }

        // Never re-run a terminal resume Task. A duplicate pending Task is safe to
        // reconcile; ProviderTaskHandler's durable Action marker forbids duplicate
        // external calls across ambiguous/crash boundaries.
        if (gaudere::work::is_terminal(claim.task->status)) {
            std::cout << "resume_task_terminal=true\nprovider_call_started=false\n";
            return 0;
        }

        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);
        gaudere::scheduling::wake::Runtime action_runtime(action_store, now);
        action_runtime.recover();

        gaudere_agent::OpenAIActivation activation(
            action_runtime, action_store, budget_store,
            options.model, options.secret, options.secret_directory);
        gaudere_agent::ResumeAfterWakeCognitionHandler cognition(
            activation.handler());
        gaudere_agent::TaskExecutor executor(work_runtime, task_store);

        const auto execution = executor.execute(
            claim.task->id, "resume-after-wake-one-shot", cognition);
        if (execution != gaudere_agent::ExecuteResult::completed) {
            std::cerr << "gaudere-resume-after-wake: execution did not complete\n";
            return 4;
        }

        const auto stored = task_store.find(claim.task->id);
        if (!stored || !gaudere::work::is_terminal(stored->status)) {
            throw std::runtime_error(
                "resume Task is not terminal after one-shot execution");
        }
        std::cout << "resume_task_terminal=true\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-resume-after-wake: " << error.what() << '\n';
        return 1;
    }
}
