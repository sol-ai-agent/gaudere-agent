#include "LocalGooseCycleBootstrap.hpp"
#include "LocalGooseCycleStore.hpp"
#include "StateLock.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

struct Arguments {
    std::string state_path;
    std::string cycle_sidecar;
};

Arguments parse_arguments(const int argc, char** argv)
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string item = argv[index];
        if (item == "--state" && index + 1 < argc) {
            arguments.state_path = argv[++index];
        } else if (item == "--cycle-sidecar" && index + 1 < argc) {
            arguments.cycle_sidecar = argv[++index];
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + item);
        }
    }
    if (arguments.state_path.empty()) throw std::invalid_argument("--state is required");
    if (arguments.cycle_sidecar.empty())
        throw std::invalid_argument("--cycle-sidecar is required");
    if (arguments.state_path == arguments.cycle_sidecar)
        throw std::invalid_argument("cycle sidecar path must differ from state path");
    return arguments;
}

const char* result_name(
    const gaudere_agent::LocalGooseCycleBootstrapResult result) noexcept
{
    using Result = gaudere_agent::LocalGooseCycleBootstrapResult;
    switch (result) {
    case Result::seeded: return "seeded";
    case Result::duplicate: return "duplicate";
    case Result::activated: return "activated";
    case Result::already_active: return "already_active";
    case Result::conflict: return "conflict";
    case Result::invalid: return "invalid";
    case Result::unavailable: return "unavailable";
    }
    return "unknown";
}

} // namespace

int main(const int argc, char** argv)
{
    try {
        const auto arguments = parse_arguments(argc, argv);
        if (!std::filesystem::is_regular_file(
                std::filesystem::symlink_status(arguments.cycle_sidecar))) {
            throw std::invalid_argument(
                "cycle sidecar must already exist as a regular non-symlink file");
        }

        gaudere_agent::StateLock state_lock(arguments.state_path);
        gaudere_agent::LocalGooseCycleStore cycle_store(arguments.cycle_sidecar);
        const auto due_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto step = gaudere_agent::activate_local_goose_cycle(
            cycle_store, due_at_ms);
        if ((step.result
                != gaudere_agent::LocalGooseCycleBootstrapResult::activated
             && step.result
                != gaudere_agent::LocalGooseCycleBootstrapResult::already_active)
            || !step.cursor || !step.cursor->due_at_ms) {
            std::cerr << "gaudere-local-goose-cycle-activate: "
                      << (step.detail.empty() ? result_name(step.result) : step.detail)
                      << '\n';
            return 3;
        }

        const Json output{
            {"due_at_ms", *step.cursor->due_at_ms},
            {"generation", step.cursor->generation},
            {"result", result_name(step.result)},
            {"revision", step.cursor->revision},
            {"schema", "gaudere.cognition.local-goose-cycle-activation.v1"},
            {"scope", step.cursor->scope},
            {"state", "scheduled"}
        };
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-cycle-activate: " << error.what() << '\n';
        return 1;
    }
}
