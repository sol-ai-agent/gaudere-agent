#include "ContinuityDeltaCheckpointInspection.hpp"
#include "LocalActivityPulse.hpp"
#include "LocalActivityPulseStore.hpp"
#include "StateLock.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;

struct Arguments {
    std::string state_path;
    std::string sidecar_path;
    std::string checkpoint_task_id;
};

Arguments parse_arguments(const int argc, char** argv)
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string item = argv[index];
        if (item == "--state" && index + 1 < argc) {
            arguments.state_path = argv[++index];
        } else if (item == "--sidecar" && index + 1 < argc) {
            arguments.sidecar_path = argv[++index];
        } else if (item == "--checkpoint" && index + 1 < argc) {
            arguments.checkpoint_task_id = argv[++index];
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + item);
        }
    }
    if (arguments.state_path.empty()) throw std::invalid_argument("--state is required");
    if (arguments.sidecar_path.empty()) throw std::invalid_argument("--sidecar is required");
    if (arguments.checkpoint_task_id.empty())
        throw std::invalid_argument("--checkpoint is required");
    if (arguments.state_path == arguments.sidecar_path)
        throw std::invalid_argument("sidecar path must differ from Core state path");
    return arguments;
}

const char* result_name(const gaudere_agent::LocalActivityPulseResult result) noexcept
{
    using Result = gaudere_agent::LocalActivityPulseResult;
    switch (result) {
    case Result::seeded: return "seeded";
    case Result::duplicate: return "duplicate";
    case Result::disabled: return "disabled";
    case Result::unseeded: return "unseeded";
    case Result::ineligible: return "ineligible";
    case Result::not_due: return "not_due";
    case Result::preparing: return "preparing";
    case Result::waiting: return "waiting";
    case Result::settled: return "settled";
    case Result::quiescent: return "quiescent";
    case Result::clock_rollback: return "clock_rollback";
    case Result::blocked: return "blocked";
    case Result::conflict: return "conflict";
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
        const auto now = [] { return std::chrono::system_clock::now(); };

        gaudere::persistence::sqlite::TaskStore task_store(arguments.state_path);
        const auto checkpoint = task_store.find(arguments.checkpoint_task_id);
        if (!checkpoint) {
            std::cerr << "gaudere-local-activity-seed: checkpoint Task not found\n";
            return 3;
        }
        const auto checkpoint_inspection =
            gaudere_agent::inspect_succeeded_continuity_delta_checkpoint(*checkpoint);
        if (!checkpoint_inspection.eligible) {
            std::cerr << "gaudere-local-activity-seed: checkpoint is not canonical: "
                      << checkpoint_inspection.detail << '\n';
            return 4;
        }

        gaudere::persistence::sqlite::ActionStore action_store(arguments.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(arguments.state_path);
        gaudere::persistence::sqlite::WakeIntentStore wake_store(arguments.state_path);
        gaudere::work::Runtime work_runtime(task_store, now);
        gaudere_agent::LocalActivityPulseStore pulse_store(arguments.sidecar_path);
        gaudere_agent::LocalActivityPulse pulse(
            pulse_store, task_store, action_store, budget_store, wake_store,
            work_runtime, now, true);

        const auto seeded = pulse.seed(arguments.checkpoint_task_id);
        if (seeded.result != gaudere_agent::LocalActivityPulseResult::seeded
            && seeded.result != gaudere_agent::LocalActivityPulseResult::duplicate) {
            std::cerr << "gaudere-local-activity-seed: "
                      << (seeded.detail.empty() ? result_name(seeded.result) : seeded.detail)
                      << '\n';
            return 5;
        }
        if (!seeded.cursor) {
            std::cerr << "gaudere-local-activity-seed: seed returned no durable cursor\n";
            return 6;
        }

        const Json output{
            {"anchor_at_ms", seeded.cursor->anchor_at_ms},
            {"anchor_checkpoint_result_sha256",
             seeded.cursor->anchor_checkpoint_result_sha256},
            {"anchor_checkpoint_task_id", seeded.cursor->anchor_checkpoint_task_id},
            {"due_at_ms", seeded.cursor->due_at_ms},
            {"generation", seeded.cursor->generation},
            {"result", result_name(seeded.result)},
            {"revision", seeded.cursor->revision},
            {"schema", "gaudere.continuity.local-observation-seed.v1"},
            {"scope", seeded.cursor->scope}
        };
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-activity-seed: " << error.what() << '\n';
        return 1;
    }
}
