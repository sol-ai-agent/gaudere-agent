#include "LocalActivityPulseStore.hpp"
#include "LocalGooseCycleBootstrap.hpp"
#include "LocalGooseCycleStore.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

struct Arguments {
    std::string state_path;
    std::string activity_sidecar;
    std::string cycle_sidecar;
};

Arguments parse_arguments(const int argc, char** argv)
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string item = argv[index];
        if (item == "--state" && index + 1 < argc) {
            arguments.state_path = argv[++index];
        } else if (item == "--activity-sidecar" && index + 1 < argc) {
            arguments.activity_sidecar = argv[++index];
        } else if (item == "--cycle-sidecar" && index + 1 < argc) {
            arguments.cycle_sidecar = argv[++index];
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + item);
        }
    }
    if (arguments.state_path.empty()) throw std::invalid_argument("--state is required");
    if (arguments.activity_sidecar.empty())
        throw std::invalid_argument("--activity-sidecar is required");
    if (arguments.cycle_sidecar.empty())
        throw std::invalid_argument("--cycle-sidecar is required");
    if (arguments.state_path == arguments.activity_sidecar
        || arguments.state_path == arguments.cycle_sidecar
        || arguments.activity_sidecar == arguments.cycle_sidecar) {
        throw std::invalid_argument("state/activity/cycle paths must be distinct");
    }
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
        gaudere_agent::StateLock state_lock(arguments.state_path);

        const auto activity =
            gaudere_agent::inspect_local_activity_pulse_sidecar(
                arguments.activity_sidecar);
        if (!activity.eligible || !activity.cursor) {
            std::cerr << "gaudere-local-goose-cycle-seed: local activity sidecar is not eligible: "
                      << activity.detail << '\n';
            return 2;
        }

        gaudere::persistence::sqlite::TaskStore task_store(arguments.state_path);
        gaudere_agent::LocalGooseCycleStore cycle_store(arguments.cycle_sidecar);
        const auto step = gaudere_agent::seed_local_goose_cycle(
            cycle_store, task_store, *activity.cursor);
        if ((step.result
                != gaudere_agent::LocalGooseCycleBootstrapResult::seeded
             && step.result
                != gaudere_agent::LocalGooseCycleBootstrapResult::duplicate)
            || !step.cursor) {
            std::cerr << "gaudere-local-goose-cycle-seed: "
                      << (step.detail.empty() ? result_name(step.result) : step.detail)
                      << '\n';
            return 3;
        }

        const Json output{
            {"anchor_observation_result_sha256",
             step.cursor->anchor_observation_result_sha256},
            {"anchor_observation_task_id",
             step.cursor->anchor_observation_task_id},
            {"generation", step.cursor->generation},
            {"result", result_name(step.result)},
            {"revision", step.cursor->revision},
            {"schema", "gaudere.cognition.local-goose-cycle-seed.v1"},
            {"scope", step.cursor->scope},
            {"state", "dormant"}
        };
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-cycle-seed: " << error.what() << '\n';
        return 1;
    }
}
