#include "CurrentCognitionCycle.hpp"
#include "ResumeContextSnapshot.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
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
constexpr std::size_t max_task_id_bytes = 1024;

struct Options {
    std::string state_path;
    std::string predecessor_task_id;
    std::string context_request_path;
    bool prepare = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " --state PATH --predecessor-task-id ID --context-request-file PATH"
        << " --prepare-after-explicit-current-cognition-go\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--predecessor-task-id" && index + 1 < argc) {
            options.predecessor_task_id = argv[++index];
        } else if (argument == "--context-request-file" && index + 1 < argc) {
            options.context_request_path = argv[++index];
        } else if (argument == "--prepare-after-explicit-current-cognition-go") {
            options.prepare = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    if (options.state_path.empty())
        throw std::invalid_argument("--state PATH is required");
    if (options.predecessor_task_id.empty()
        || options.predecessor_task_id.size() > max_task_id_bytes) {
        throw std::invalid_argument("--predecessor-task-id ID must be 1..1024 bytes");
    }
    if (options.context_request_path.empty())
        throw std::invalid_argument("--context-request-file PATH is required");
    if (!options.prepare) {
        throw std::invalid_argument(
            "--prepare-after-explicit-current-cognition-go is required");
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
    if (!input) throw std::runtime_error("could not open context request file");
    std::string content(static_cast<std::size_t>(bytes), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(content.size())) {
        throw std::runtime_error("could not read complete context request file");
    }
    return content;
}

const char* record_name(const gaudere_agent::ResumeContextSnapshotRecordResult result)
{
    using Result = gaudere_agent::ResumeContextSnapshotRecordResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::invalid: return "invalid";
    case Result::conflict: return "conflict";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* claim_name(const gaudere_agent::CurrentCognitionClaimResult result)
{
    using Result = gaudere_agent::CurrentCognitionClaimResult;
    switch (result) {
    case Result::accepted: return "accepted";
    case Result::duplicate: return "duplicate";
    case Result::disabled: return "disabled";
    case Result::predecessor_not_found: return "predecessor_not_found";
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

        // Validate caller-controlled bytes before opening durable state. No provider,
        // Action, budget, WakeIntent, network or secret object exists in this binary.
        const auto request_json = read_context_request(options.context_request_path);
        const auto request =
            gaudere_agent::inspect_resume_context_snapshot_request(request_json);
        if (!request.eligible) {
            throw std::invalid_argument(
                "invalid context request: " + request.detail);
        }

        gaudere_agent::StateLock state_lock(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::work::Runtime work_runtime(task_store, now);
        work_runtime.recover();

        gaudere_agent::ResumeContextSnapshotRecorder recorder(
            task_store, work_runtime, now);
        const auto recorded = recorder.record(request.canonical_request);
        if ((recorded.result
                 != gaudere_agent::ResumeContextSnapshotRecordResult::accepted
             && recorded.result
                 != gaudere_agent::ResumeContextSnapshotRecordResult::duplicate)
            || !recorded.task) {
            std::cerr << "gaudere-current-cognition-prepare: snapshot failed: "
                      << recorded.detail << '\n';
            return 3;
        }

        gaudere_agent::CurrentCognitionCycle cycle(
            task_store, work_runtime, now, true);
        const auto claimed = cycle.claim(
            options.predecessor_task_id, recorded.task->id);
        if ((claimed.result != gaudere_agent::CurrentCognitionClaimResult::accepted
             && claimed.result
                 != gaudere_agent::CurrentCognitionClaimResult::duplicate)
            || !claimed.task) {
            std::cerr << "gaudere-current-cognition-prepare: claim failed: "
                      << claimed.detail << '\n';
            return 4;
        }

        std::cout << "prepared=true\n"
                  << "predecessor_task_id=" << options.predecessor_task_id << '\n'
                  << "snapshot_result=" << record_name(recorded.result) << '\n'
                  << "snapshot_task_id=" << recorded.task->id << '\n'
                  << "claim=" << claim_name(claimed.result) << '\n'
                  << "current_task_id=" << claimed.task->id << '\n'
                  << "provider_effects=0\n"
                  << "current_task_executed=false\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-current-cognition-prepare: "
                  << error.what() << '\n';
        return 1;
    }
}
