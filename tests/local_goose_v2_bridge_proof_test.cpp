#include "LocalGooseV2BridgeProof.hpp"
#include "LocalGooseCycle.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gaudere_agent;
using Task = gaudere::work::Task;
using Result = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using Json = nlohmann::json;
using Status = LocalGooseV2BridgeStatus;

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
    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = Result{local_continuity_observation_content_type,
                         task.input, {}, {}};
    assert(canonical_local_continuity_observation_success(task));
    return task;
}

Task final_anchor()
{
    const auto a = observation(1);
    const auto b = observation(2, a);
    return observation(3, b);
}

std::string decision(std::string kind = "idle",
                     std::optional<std::int64_t> delay = std::nullopt)
{
    return Json{
        {"assessment", "PRIVATE-HISTORICAL-ASSESSMENT"},
        {"decision", kind},
        {"next_wake_after_ms", delay ? Json(*delay) : Json(nullptr)},
        {"openai_request", kind == "request_openai"
            ? Json("PRIVATE-PROVIDER-REQUEST") : Json(nullptr)},
        {"reason", "PRIVATE-HISTORICAL-REASON"},
        {"schema", local_goose_decision_schema}
    }.dump();
}

struct Fixture {
    Task anchor = final_anchor();
    std::string model = hex('e');
    std::vector<Task> history;
    std::map<std::string, Task> stored;
    LocalGooseCycleCursor cursor;
    int cursor_reads = 0;
    int task_reads = 0;
    bool changed_revision = false;
    bool changed_same_revision = false;
    bool throw_task = false;
    bool throw_cursor = false;

    Fixture()
    {
        cursor.revision = 3;
        cursor.anchor_observation_task_id = anchor.id;
        cursor.anchor_observation_result_sha256 =
            sha256_hex(anchor.result->output);
        stored.emplace(anchor.id, anchor);
        push();
    }

    void push(const std::string& kind = "idle",
              std::optional<std::int64_t> delay = std::nullopt)
    {
        const auto gen = history.size() + 1;
        const auto ms = static_cast<std::int64_t>(gen) * 10000;
        const auto previous = history.empty()
            ? std::nullopt : std::optional<Task>{history.back()};
        auto task = make_local_goose_cycle_task(
            anchor, hex('e'), gen, ms, ms + 1, previous);
        task.status = TaskStatus::succeeded;
        task.result = Result{local_goose_decision_content_type,
                             decision(kind, delay), {}, {}};
        assert(canonical_local_goose_cycle_success(task));
        stored.emplace(task.id, task);
        history.push_back(task);
        cursor.generation = history.size() + 1;
        cursor.revision += 1;
        cursor.predecessor_task_id = task.id;
        cursor.predecessor_result_sha256 = sha256_hex(task.result->output);
    }

    LocalGooseV2BridgeInspection inspect()
    {
        cursor_reads = task_reads = 0;
        return inspect_local_goose_v2_dormant_bridge(
            [&]() -> std::optional<LocalGooseCycleCursor> {
                if (throw_cursor) throw std::runtime_error("hidden-cursor-error");
                ++cursor_reads;
                auto copy = cursor;
                if (cursor_reads == 2 && changed_revision) ++copy.revision;
                if (cursor_reads == 2 && changed_same_revision)
                    copy.anchor_observation_result_sha256 = hex('f');
                return copy;
            },
            [&](const std::string& id) -> std::optional<Task> {
                ++task_reads;
                if (throw_task) throw std::runtime_error("hidden-task-error");
                auto it = stored.find(id);
                if (it == stored.end()) return std::nullopt;
                return it->second;
            },
            model);
    }

    void expect(Status status)
    {
        const auto result = inspect();
        assert(result.status == status && !result.evidence);
        assert(cursor_reads <= 2 && task_reads <= 5);
    }
};
} // namespace

int main()
{
    {
        Fixture f;
        const auto a = f.inspect();
        assert(a.status == Status::verified_dormant_no_authority);
        assert(a.evidence);
        assert(a.evidence->cursor_revision == f.cursor.revision);
        assert(a.evidence->next_generation == 2);
        assert(a.evidence->predecessor_task_id == f.history[0].id);
        assert(a.evidence->predecessor_result_sha256
            == *f.cursor.predecessor_result_sha256);
        assert(a.evidence->bounded_memory_entries == 1);
        assert(!a.evidence->earlier_generations_omitted);
        assert(a.evidence->bounded_memory_sha256.size() == 64);
        assert(f.cursor_reads == 2 && f.task_reads == 2);
        const auto b = f.inspect(); // replay after simulated read-only restart
        assert(b.status == a.status && b.evidence);
        assert(b.evidence->bounded_memory_sha256
            == a.evidence->bounded_memory_sha256);
        assert(b.evidence->predecessor_task_id
            == a.evidence->predecessor_task_id);
        assert(b.evidence->cursor_revision == a.evidence->cursor_revision);
        assert(b.evidence->bounded_memory_sha256.find("PRIVATE-")
            == std::string::npos);
        assert(f.cursor.state == LocalGooseCycleState::dormant);
        assert(!f.cursor.due_at_ms && f.cursor.current_task_id.empty());
    }
    {
        Fixture f;
        f.stored[f.history.back().id].result->output =
            decision("request_openai");
        f.cursor.predecessor_result_sha256 =
            sha256_hex(f.stored.at(f.history.back().id).result->output);
        const auto b = f.inspect();
        assert(b.status == Status::verified_dormant_no_authority);
        assert(b.evidence && b.evidence->bounded_memory_entries == 1);
        // The private provider request itself never enters the evidence.
        assert(b.evidence->bounded_memory_sha256.find("PRIVATE-")
            == std::string::npos);
    }
    {
        Fixture f;
        for (int i = 0; i < 4; ++i) f.push();
        const auto e = f.inspect();
        assert(e.status == Status::verified_dormant_no_authority);
        assert(e.evidence && e.evidence->next_generation == 6);
        assert(e.evidence->bounded_memory_entries == 4);
        assert(e.evidence->earlier_generations_omitted);
        assert(f.task_reads == 5 && f.cursor_reads == 2);
        // Generations outside the bounded window are not read.
        f.stored.erase(f.history[0].id);
        assert(f.inspect().status == Status::verified_dormant_no_authority);
        // A missing required intermediate Task fails closed, no skip.
        f.stored.erase(f.history[2].id);
        f.expect(Status::unavailable);
    }
    { Fixture f; f.changed_revision = true; f.expect(Status::snapshot_changed); }
    { Fixture f; f.changed_same_revision = true; f.expect(Status::snapshot_changed); }
    { Fixture f; f.throw_cursor = true; f.expect(Status::unavailable); }
    { Fixture f; f.throw_task = true; f.expect(Status::unavailable); }
    { Fixture f; f.stored.erase(f.anchor.id); f.expect(Status::unavailable); }
    { Fixture f; f.stored.erase(f.history[0].id); f.expect(Status::unavailable); }
    { Fixture f; f.model = hex('f'); f.expect(Status::noncanonical); }
    { Fixture f; f.cursor.scope = "untrusted-scope"; f.expect(Status::noncanonical); }
    { Fixture f; f.cursor.predecessor_result_sha256 = hex('f');
      f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.history[0].id).result->output += "\n";
      f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.history[0].id).input += " ";
      f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.history[0].id).status = TaskStatus::failed;
      f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.anchor.id).result->output += " ";
      f.expect(Status::noncanonical); }
    { Fixture f; f.cursor.generation = 3; f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.history[0].id).result->output =
          decision("continue_local");
      f.cursor.predecessor_result_sha256 =
          sha256_hex(f.stored.at(f.history[0].id).result->output);
      f.expect(Status::noncanonical); }
    { Fixture f; f.stored.at(f.history[0].id).result->output =
          decision("idle", 5000);
      f.cursor.predecessor_result_sha256 =
          sha256_hex(f.stored.at(f.history[0].id).result->output);
      f.expect(Status::noncanonical); }
    { Fixture f; f.cursor.generation = 0;
      f.cursor.predecessor_task_id.reset();
      f.cursor.predecessor_result_sha256.reset();
      f.expect(Status::no_predecessor);
      assert(f.task_reads == 0); }
    { Fixture f; f.cursor.state = LocalGooseCycleState::scheduled;
      f.cursor.due_at_ms = 20000; f.expect(Status::not_applicable);
      assert(f.task_reads == 0); }
    { Fixture f; f.cursor.state = LocalGooseCycleState::blocked;
      f.cursor.blocked_reason = "test"; f.expect(Status::not_applicable);
      assert(f.task_reads == 0); }
    { Fixture f; f.cursor.state = LocalGooseCycleState::prepared;
      f.cursor.due_at_ms = 20000;
      f.cursor.captured_at_ms = 20001;
      f.cursor.current_task_id = f.history[0].id;
      f.expect(Status::noncanonical);
      assert(f.task_reads == 0); }
    std::cout << "V1 dormant cursor V2 bridge read-only proof: ok\n";
    return 0;
}
