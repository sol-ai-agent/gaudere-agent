#include "CurrentCognitionTaskInspection.hpp"

#include "CurrentCognitionCycle.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr const char* linkage_marker = "Durable cognition linkage JSON:\n";

} // namespace

CurrentCognitionTaskInspection inspect_current_cognition_task(
    const gaudere::work::Task& task) noexcept
{
    if (!valid_current_cognition_task(task)) {
        return {false, {}, {}, {}, {}, -1,
                "current cognition Task is non-canonical"};
    }

    try {
        // The canonical prompt prefix contains this marker exactly once before all
        // caller-controlled lineage data. Use the first occurrence so snapshot text
        // cannot redirect extraction by repeating the marker later in the prompt.
        const auto marker = task.input.find(linkage_marker);
        if (marker == std::string::npos) {
            return {false, {}, {}, {}, {}, -1,
                    "canonical current cognition linkage marker is missing"};
        }
        const auto linkage_bytes = task.input.substr(
            marker + std::string{linkage_marker}.size());
        const auto linkage = Json::parse(linkage_bytes);

        const auto predecessor_task_id =
            linkage.at("predecessor_task_id").get<std::string>();
        const auto predecessor_decision =
            linkage.at("predecessor_decision").dump();
        const auto snapshot_task_id =
            linkage.at("snapshot_task_id").get<std::string>();
        const auto snapshot_capsule = linkage.at("snapshot").dump();
        const auto& captured = linkage.at("snapshot").at("captured_at_ms");
        if (!(captured.is_number_integer() || captured.is_number_unsigned())) {
            return {false, {}, {}, {}, {}, -1,
                    "canonical current cognition capture time is invalid"};
        }
        const auto captured_at_ms = captured.get<std::int64_t>();
        if (captured_at_ms < 0) {
            return {false, {}, {}, {}, {}, -1,
                    "canonical current cognition capture time is negative"};
        }

        return {true, predecessor_task_id, predecessor_decision,
                snapshot_task_id, snapshot_capsule, captured_at_ms, {}};
    } catch (...) {
        // valid_current_cognition_task() already proved the contract. Reaching this
        // branch therefore means extraction itself could not reproduce that proof.
        return {false, {}, {}, {}, {}, -1,
                "canonical current cognition lineage could not be extracted"};
    }
}

} // namespace gaudere_agent
