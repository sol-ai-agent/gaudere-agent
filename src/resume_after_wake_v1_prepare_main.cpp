#include "ResumeAfterWakeV1Prepare.hpp"
#include "StateLock.hpp"
#include "WakeSourceDecision.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uintmax_t max_context_request_file_bytes = 32 * 1024;

struct Options {
    std::string state_path;
    std::string context_request_path;
    bool prepare = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " --state PATH --context-request-file PATH"
        << " --prepare-after-explicit-resume-v1-go\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--context-request-file" && index + 1 < argc) {
            options.context_request_path = argv[++index];
        } else if (argument == "--prepare-after-explicit-resume-v1-go") {
            options.prepare = true;
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
    if (options.context_request_path.empty()) {
        throw std::invalid_argument("--context-request-file PATH is required");
    }
    if (!options.prepare) {
        throw std::invalid_argument(
            "--prepare-after-explicit-resume-v1-go is required");
    }
    return options;
}

std::string read_context_request(const std::string& path_text)
{
    const std::filesystem::path path(path_text);
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            "context request must be a regular non-symlink file");
    }
    const auto bytes = std::filesystem::file_size(path);
    if (bytes == 0 || bytes > max_context_request_file_bytes) {
        throw std::invalid_argument(
            "context request file must be 1..32768 bytes");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open context request file");
    }
    std::string content(static_cast<std::size_t>(bytes), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(content.size())) {
        throw std::runtime_error("could not read complete context request file");
    }
    return content;
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

const char* claim_name(
    const gaudere_agent::ResumeAfterWakeV1ClaimResult result)
{
    using Result = gaudere_agent::ResumeAfterWakeV1ClaimResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::disabled: return "disabled";
    case Result::wake_not_found: return "wake_not_found";
    case Result::snapshot_not_found: return "snapshot_not_found";
    case Result::ineligible: return "ineligible";
    case Result::stale: return "stale";
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

        // Mutation is possible only after the explicit token was parsed and this
        // process obtained exclusive ownership of the state database.
        gaudere_agent::StateLock state_lock(options.state_path);
        const auto request_json = read_context_request(options.context_request_path);

        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::WakeIntentStore wake_store(options.state_path);
        const auto wake_id = sole_wake_id(wake_store);
        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::work::Runtime work_runtime(task_store, now);
        work_runtime.recover();

        gaudere_agent::ResumeAfterWakeV1Prepare prepare(
            task_store, wake_store, work_runtime, now, true,
            [](const std::string_view phase) {
                std::cout << "phase=" << phase << '\n';
            });
        const auto result = prepare.prepare(wake_id, request_json);
        if (!result.prepared || !result.selection_task || !result.snapshot_task
            || !result.claim.task) {
            std::cerr << "gaudere-resume-after-wake-v1-prepare: preparation failed: "
                      << result.detail << '\n';
            return 3;
        }

        std::cout << "prepared=true\n"
                  << "duplicate=" << (result.duplicate ? "true" : "false") << '\n'
                  << "wake_id=" << wake_id << '\n'
                  << "selection_task_id=" << result.selection_task->id << '\n'
                  << "snapshot_task_id=" << result.snapshot_task->id << '\n'
                  << "resume_task_id=" << result.claim.task->id << '\n'
                  << "claim=" << claim_name(result.claim.result) << '\n'
                  << "provider_effects=0\n"
                  << "resume_task_executed=false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-resume-after-wake-v1-prepare: "
                  << error.what() << '\n';
        return 1;
    }
}
