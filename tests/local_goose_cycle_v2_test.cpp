#include "LocalGooseCycleV2.hpp"
#include "LocalGooseCycle.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
using namespace gaudere_agent;
using Task = gaudere::work::Task;
using Result = gaudere::work::TaskResult;
using Status = gaudere::work::TaskStatus;
using Json = nlohmann::json;

std::string hex(char c) { return std::string(64, c); }

Task observation(std::uint32_t generation,
                 const std::optional<Task>& previous = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 10;
    facts.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('a');
    if (previous) {
        facts.predecessor_observation_task_id = previous->id;
        facts.predecessor_observation_result_sha256 =
            sha256_hex(previous->result->output);
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
    auto t = make_local_continuity_observation_task(facts);
    t.status = Status::succeeded;
    t.result = Result{local_continuity_observation_content_type,
                      t.input, {}, {}};
    assert(canonical_local_continuity_observation_success(t));
    return t;
}

Task anchor()
{
    const auto first = observation(1);
    const auto second = observation(2, first);
    return observation(3, second);
}

std::string decision(std::string kind = "idle")
{
    return Json{
        {"assessment", "one generation of local cognition"},
        {"decision", kind},
        {"next_wake_after_ms", nullptr},
        {"openai_request", kind == "request_openai"
            ? Json("DO-NOT-LEAK-PRIVATE-PROVIDER-REQUEST") : Json(nullptr)},
        {"reason", "reflect before next opportunity"},
        {"schema", local_goose_decision_schema}
    }.dump();
}

struct Fixture {
    Task initial = anchor();
    std::string model = hex('e');
    std::vector<Task> history;
    std::map<std::string, Task> stored;
    unsigned reads = 0;

    void push(std::string kind = "idle")
    {
        const auto generation = history.size() + 1;
        const auto ms = static_cast<std::int64_t>(10000 * generation);
        const auto previous = history.empty()
            ? std::nullopt : std::optional<Task>{history.back()};
        auto task = make_local_goose_cycle_task(
            initial, model, generation, ms, ms + 1, previous);
        task.status = Status::succeeded;
        task.result = Result{local_goose_decision_content_type,
                             decision(std::move(kind)), {}, {}};
        assert(canonical_local_goose_cycle_success(task));
        stored[task.id] = task;
        history.push_back(std::move(task));
    }

    std::optional<Task> read(const std::string& id)
    {
        ++reads;
        const auto it = stored.find(id);
        return it == stored.end()
            ? std::nullopt : std::optional<Task>{it->second};
    }

    Task next()
    {
        const auto generation = history.size() + 1;
        const auto ms = static_cast<std::int64_t>(10000 * generation);
        reads = 0;
        return make_local_goose_cycle_v2_task(
            initial, model, generation, ms, ms + 1, history.back(),
            [this](const std::string& id) { return read(id); });
    }
};

bool throws_invalid(const std::function<void()>& work)
{
    try { work(); } catch (const std::invalid_argument&) { return true; }
    return false;
}

void resign(Task& task)
{
    const auto digest = sha256_hex(task.input);
    task.id = std::string(local_goose_cycle_v2_task_prefix) + digest;
    task.idempotency_key = std::string(local_goose_cycle_v2_task_prefix)
        + "input:" + digest;
}
} // namespace

int main()
{
    {
        Fixture f;
        f.push("request_openai");
        const auto v2 = f.next();
        const auto inspection = inspect_local_goose_cycle_v2_task(v2);
        assert(inspection.eligible && inspection.generation == 2);
        assert(f.reads == 0);
        assert(v2.kind == local_goose_cycle_v2_task_kind);
        assert(v2.id != f.history[0].id);
        assert(v2.input.find("DO-NOT-LEAK") == std::string::npos);
        assert(v2.input.find("openai_request") == std::string::npos);
        const auto frozen = Json::parse(v2.input);
        assert(frozen.at("schema") == local_goose_cycle_v2_schema);
        assert(frozen.at("memory").at("entries").size() == 1);
        assert(frozen.at("memory").at("entries")[0].at("decision")
            == "request_openai");
        assert(frozen.dump() == v2.input);
        const auto prompt = local_goose_cycle_v2_prompt(inspection);
        assert(prompt.find("UNTRUSTED historical") != std::string::npos);
        assert(prompt.find("DO-NOT-LEAK") == std::string::npos);
        assert(prompt.size() <= 48 * 1024);
        const auto identical = f.next();
        assert(identical.id == v2.id && identical.input == v2.input);

        Task corrupt = v2;
        corrupt.input += "\n";
        assert(!inspect_local_goose_cycle_v2_task(corrupt).eligible);
        corrupt = v2;
        corrupt.id += "0";
        assert(!inspect_local_goose_cycle_v2_task(corrupt).eligible);
        corrupt = v2;
        auto json = Json::parse(corrupt.input);
        json["memory"]["entries"][0]["task_id"] =
            std::string(local_goose_cycle_task_prefix) + hex('f');
        corrupt.input = json.dump();
        resign(corrupt);
        assert(!inspect_local_goose_cycle_v2_task(corrupt).eligible);
        assert(throws_invalid([] {
            (void)local_goose_cycle_v2_prompt(LocalGooseCycleV2Inspection{});
        }));
    }
    {
        Fixture f;
        for (int i = 0; i < 5; ++i) f.push();
        auto v2 = f.next();
        assert(f.reads == 3);
        const auto inspection = inspect_local_goose_cycle_v2_task(v2);
        assert(inspection.eligible && inspection.generation == 6);
        const auto memory = Json::parse(inspection.memory_json);
        assert(memory.at("earlier_omitted") == true);
        assert(memory.at("entries").size() == 4);
        assert(memory.at("entries")[0].at("generation") == 2);
        assert(memory.at("entries")[3].at("generation") == 5);
        auto json = Json::parse(v2.input);
        json["memory"]["entries"][0]["generation"] = 9;
        v2.input = json.dump();
        resign(v2);
        assert(!inspect_local_goose_cycle_v2_task(v2).eligible);
        // Even with a rehashed Task ID, shortening the recent history
        // is invalid; only entries older than the fixed window are omitted.
        auto shortened = f.next();
        auto shrink = Json::parse(shortened.input);
        shrink["memory"]["entries"].erase(
            shrink["memory"]["entries"].begin());
        shortened.input = shrink.dump();
        resign(shortened);
        assert(!inspect_local_goose_cycle_v2_task(shortened).eligible);
        // A missing Task *inside* the bounded window cannot be skipped.
        f.stored.erase(f.history[2].id);
        assert(throws_invalid([&] { (void)f.next(); }));
    }
    {
        Fixture f;
        f.push();
        auto invalid = f.history.back();
        invalid.result->output += "\n";
        assert(throws_invalid([&] {
            (void)make_local_goose_cycle_v2_task(
                f.initial, f.model, 2, 20000, 20001, invalid,
                [&](const std::string& id) { return f.read(id); });
        }));
        assert(throws_invalid([&] {
            (void)make_local_goose_cycle_v2_task(
                f.initial, f.model, 1, 10000, 10001, f.history.back(),
                [&](const std::string& id) { return f.read(id); });
        }));
    }
    std::cout << "source-only frozen local Goose cycle V2: ok\n";
    return 0;
}
