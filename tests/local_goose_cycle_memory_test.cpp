#include "LocalGooseCycleMemory.hpp"
#include "LocalGooseCycle.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdlib>

// Keep the failing invariant visible in GitHub Actions' captured test log.
#undef assert
#define assert(expression) do { \
    if (!(expression)) { \
        std::cerr << "local_goose_cycle_memory_test.cpp:" << __LINE__ \
                  << " failed: " #expression << '\n'; \
        std::abort(); \
    } \
} while (false)
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using Json = nlohmann::json;

std::string hex(char c) { return std::string(64, c); }

Task observation(std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 10;
    facts.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('a');
    if (generation > 1) {
        assert(predecessor && predecessor->result);
        facts.predecessor_observation_task_id = predecessor->id;
        facts.predecessor_observation_result_sha256 =
            sha256_hex(predecessor->result->output);
    }
    facts.provider_scope = "provider.call:openai.responses";
    facts.provider_total = 10;
    facts.provider_limit = 12;
    facts.predecessor_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('b');
    facts.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('c');
    facts.historical_wake_scope = "cognition.reflect.wake.v0";
    facts.historical_wake_sha256 = hex('d');
    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{local_continuity_observation_content_type,
                             task.input, {}, {}};
    assert(canonical_local_continuity_observation_success(task));
    return task;
}

Task final_observation()
{
    const auto a = observation(1);
    const auto b = observation(2, a);
    return observation(3, b);
}

std::string decision(std::string assessment, std::string reason,
                     std::string kind = "idle")
{
    return Json{
        {"assessment", std::move(assessment)},
        {"decision", kind},
        {"next_wake_after_ms", nullptr},
        {"openai_request", kind == "request_openai"
            ? Json("DO-NOT-LEAK-PRIVATE-REQUEST") : Json{nullptr}},
        {"reason", std::move(reason)},
        {"schema", local_goose_decision_schema}
    }.dump();
}

Task succeeded(Task t, std::string result)
{
    t.status = TaskStatus::succeeded;
    t.result = TaskResult{local_goose_decision_content_type,
                           std::move(result), {}, {}};
    assert(canonical_local_goose_cycle_success(t));
    return t;
}

struct Fixture {
    Task anchor = final_observation();
    std::string model = hex('e');
    std::map<std::string, Task> stored;
    std::vector<Task> history;
    int reads = 0;

    void push(std::string assessment = "present", std::string reason = "ok",
              std::string kind = "idle")
    {
        const auto generation = history.size() + 1;
        const auto ms = static_cast<std::int64_t>(generation * 10000);
        const auto prior = history.empty()
            ? std::nullopt : std::optional<Task>{history.back()};
        auto current = succeeded(make_local_goose_cycle_task(
            anchor, model, generation, ms, ms + 1, prior),
            decision(std::move(assessment), std::move(reason), std::move(kind)));
        stored[current.id] = current;
        history.push_back(std::move(current));
    }

    LocalGooseCycleMemory read()
    {
        reads = 0;
        return build_local_goose_cycle_memory(
            history.back(),
            [&](const std::string& id) -> std::optional<Task> {
                ++reads;
                const auto it = stored.find(id);
                if (it == stored.end()) return std::nullopt;
                return it->second;
            });
    }
};
} // namespace

int main()
{
    using Status = LocalGooseCycleMemoryStatus;
    {
        Fixture f;
        f.push("first", "local", "request_openai");
        const auto memory = f.read();
        assert(memory.status == Status::verified);
        assert(memory.included == 1 && !memory.earlier_omitted);
        assert(f.reads == 0);
        assert(memory.canonical_json.find("DO-NOT-LEAK") == std::string::npos);
        const auto root = Json::parse(memory.canonical_json);
        assert(root.at("schema") == "gaudere.cognition.local-goose-memory.v1");
        assert(root.at("entries").size() == 1);
        assert(root.at("entries")[0].at("decision") == "request_openai");
        assert(root.at("entries")[0].at("generation") == 1);
        assert(root.at("entries")[0].at("next_wake_after_ms").is_null());
        assert(root.dump() == memory.canonical_json);
        const auto again = f.read();
        assert(again.canonical_json == memory.canonical_json);
    }
    {
        Fixture f;
        for (int i = 0; i < 4; ++i)
            f.push("assessment " + std::to_string(i),
                   "reason " + std::to_string(i));
        const auto m = f.read();
        assert(m.status == Status::verified && m.included == 4);
        assert(!m.earlier_omitted && f.reads == 3);
        const auto root = Json::parse(m.canonical_json);
        assert(root.at("entries").size() == 4);
        for (int i = 0; i < 4; ++i) {
            assert(root.at("entries")[i].at("generation") == i + 1);
            assert(root.at("entries")[i].at("task_id")
                == f.history[static_cast<std::size_t>(i)].id);
        }
        f.stored.erase(f.history[1].id);
        assert(f.read().status == Status::unavailable);
    }
    {
        Fixture f;
        for (int i = 0; i < 5; ++i) f.push();
        const auto m = f.read();
        assert(m.status == Status::verified && m.included == 4);
        assert(m.earlier_omitted && f.reads == 3);
        const auto entries = Json::parse(m.canonical_json).at("entries");
        assert(entries[0].at("generation") == 2);
        assert(entries[3].at("generation") == 5);
        // The bounded window neither audits nor reads the omitted generation.
        f.stored.erase(f.history[0].id);
        assert(f.read().status == Status::verified);
        // But every intermediate link within the window is verified.
        f.stored.at(f.history[2].id).result->output =
            decision("tampered", "changed");
        assert(f.read().status == Status::noncanonical);
    }
    {
        Fixture f;
        f.push();
        f.push();
        f.stored.at(f.history[0].id).status = TaskStatus::failed;
        assert(f.read().status == Status::noncanonical);
        f.stored[f.history[0].id] = f.history[0];
        f.stored.at(f.history[0].id).result->output += "\n";
        assert(f.read().status == Status::noncanonical);
        f.stored[f.history[0].id] = f.history[0];
        f.stored.at(f.history[0].id).input += " ";
        assert(f.read().status == Status::noncanonical);
    }
    {
        Fixture f;
        const std::string long_reason = std::string(1023, 'a') + "\xc3\xa9";
        f.push("private sentiment", long_reason);
        auto m = f.read();
        assert(m.status == Status::verified);
        const auto entry = Json::parse(m.canonical_json).at("entries")[0];
        assert(entry.at("reason_truncated") == true);
        assert(entry.at("reason").get<std::string>().size() == 1023);
        assert(entry.at("assessment_truncated") == false);
    }
    std::cout << "local Goose bounded memory: ok\n";
    return 0;
}
