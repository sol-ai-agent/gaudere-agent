#include "LocalGooseCycleMemory.hpp"

#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {
using Task = gaudere::work::Task;
using Json = nlohmann::json;
using Status = LocalGooseCycleMemoryStatus;
constexpr std::size_t max_entries = 4;
constexpr std::size_t max_excerpt_bytes = 1024;
constexpr std::size_t max_memory_bytes = 12 * 1024;

LocalGooseCycleMemory fail(Status s) { return {s, {}, 0, false}; }

// Inspect_local_goose_decision has already parsed the source as valid UTF-8
// JSON. Cut only at codepoint boundaries; never split a multibyte character.
std::pair<std::string, bool> excerpt(const std::string& text)
{
    std::size_t pos = 0;
    while (pos < text.size() && pos < max_excerpt_bytes) {
        const auto first = static_cast<unsigned char>(text[pos]);
        const std::size_t width = first < 0x80 ? 1 : first < 0xe0 ? 2
            : first < 0xf0 ? 3 : 4;
        if (pos + width > max_excerpt_bytes) break;
        pos += width;
    }
    return {text.substr(0, pos), pos != text.size()};
}

} // namespace

LocalGooseCycleMemory build_local_goose_cycle_memory(
    const Task& latest, const LocalGooseCycleMemoryRead& read_predecessor) noexcept
{
    try {
        // Source-only V1 lineage support. A future V2 transition must have its
        // own canonical Task contract and explicit migration review.
        const auto first = inspect_local_goose_cycle_task(latest);
        if (!first.eligible || !canonical_local_goose_cycle_success(latest)
            || !latest.result)
            return fail(Status::noncanonical);

        const auto anchor = first.anchor_observation_task_id;
        const auto anchor_sha = first.anchor_observation_result_sha256;
        const auto model = first.model_sha256;
        auto task = latest;
        auto expected_generation = first.generation;
        std::optional<std::string> expected_id;
        std::optional<std::string> expected_sha;
        std::optional<std::string> expected_decision;
        std::vector<Json> reverse_entries;
        bool omitted = false;

        for (std::size_t i = 0; i < max_entries; ++i) {
            if (!task.result || !canonical_local_goose_cycle_success(task))
                return fail(Status::noncanonical);
            const auto facts = inspect_local_goose_cycle_task(task);
            if (!facts.eligible || facts.generation != expected_generation
                || facts.anchor_observation_task_id != anchor
                || facts.anchor_observation_result_sha256 != anchor_sha
                || facts.model_sha256 != model
                || (expected_id && task.id != *expected_id)
                || (expected_sha && sha256_hex(task.result->output) != *expected_sha))
                return fail(Status::noncanonical);

            const auto decision = inspect_local_goose_decision(task.result->output);
            if (!decision.eligible ||
                (expected_decision
                 && decision.decision.canonical_json != *expected_decision))
                return fail(Status::noncanonical);
            const auto [assessment, assessment_cut] =
                excerpt(decision.decision.assessment);
            const auto [reason, reason_cut] =
                excerpt(decision.decision.reason);
            reverse_entries.push_back(Json{
                {"assessment", assessment},
                {"assessment_truncated", assessment_cut},
                {"decision", decision.decision.decision},
                {"generation", facts.generation},
                {"next_wake_after_ms", decision.decision.next_wake_after_ms},
                {"reason", reason},
                {"reason_truncated", reason_cut},
                {"result_sha256", sha256_hex(task.result->output)},
                {"task_id", task.id}
            });

            if (facts.generation == 1) break;
            if (!facts.predecessor) return fail(Status::noncanonical);
            if (i + 1 == max_entries) {
                omitted = true;
                break;
            }
            const auto& link = *facts.predecessor;
            expected_id = link.task_id;
            expected_sha = link.result_sha256;
            expected_decision = link.decision.canonical_json;
            --expected_generation;
            const auto previous = read_predecessor(*expected_id);
            if (!previous) return fail(Status::unavailable);
            task = *previous;
        }

        std::reverse(reverse_entries.begin(), reverse_entries.end());
        const Json encoded{
            {"earlier_omitted", omitted},
            {"entries", reverse_entries},
            {"schema", "gaudere.cognition.local-goose-memory.v1"}
        };
        const auto bytes = encoded.dump();
        if (bytes.size() > max_memory_bytes)
            return fail(Status::oversized);
        return {Status::verified, bytes, reverse_entries.size(), omitted};
    } catch (...) {
        return fail(Status::unavailable);
    }
}
} // namespace gaudere_agent
