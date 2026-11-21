#include "LocalGooseSettledInspection.hpp"
#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <cassert>
#include <map>
#include <stdexcept>
#include <iostream>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(const char c) { return std::string(64, c); }

Task observation(const std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 10;
    facts.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:" + hex('a');
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
    const auto one = observation(1);
    const auto two = observation(2, one);
    return observation(3, two);
}


using Status = LocalGooseSettledStatus;
using Decision = LocalGooseSettledDecision;

std::string decision(const std::string& kind, const std::string& delay = "null")
{
    return std::string{"{\"assessment\":\"private sentinel\",\"decision\":\""}
        + kind + "\",\"next_wake_after_ms\":" + delay +
        ",\"openai_request\":" + (kind == "request_openai" ? "\"private request\"" : "null") +
        ",\"reason\":\"private reason\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}";
}

Task succeeded(Task task, const std::string& output)
{
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{local_goose_decision_content_type, output, {}, {}};
    return task;
}

struct Fixture {
    Task anchor = final_observation();
    std::string model = hex('e');
    Task first = succeeded(make_local_goose_cycle_task(anchor, model, 1, 10000, 10001),
                           decision("idle"));
    LocalGooseCycleCursor cursor;
    std::map<std::string, Task> tasks;
    int cursor_reads = 0;
    int task_reads = 0;
    bool change = false;
    bool throw_read = false;

    Fixture()
    {
        cursor.revision = 3;
        cursor.generation = 2;
        cursor.anchor_observation_task_id = anchor.id;
        cursor.anchor_observation_result_sha256 = sha256_hex(anchor.result->output);
        tasks.emplace(anchor.id, anchor);
        set_predecessor(first);
    }

    void set_predecessor(const Task& task)
    {
        cursor.predecessor_task_id = task.id;
        cursor.predecessor_result_sha256 = sha256_hex(task.result->output);
        tasks.insert_or_assign(task.id, task);
    }

    LocalGooseSettledInspection read()
    {
        cursor_reads = task_reads = 0;
        return inspect_local_goose_settled_decision(
            [&]() -> std::optional<LocalGooseCycleCursor> {
                ++cursor_reads;
                auto snapshot = cursor;
                if (change && cursor_reads == 2) ++snapshot.revision;
                return snapshot;
            },
            [&](const std::string& id) -> std::optional<Task> {
                ++task_reads;
                if (throw_read) throw std::runtime_error("private exception");
                const auto found = tasks.find(id);
                if (found == tasks.end()) return std::nullopt;
                return found->second;
            }, model);
    }

    void expect(Status status)
    {
        const auto result = read();
        assert(result.status == status);
        assert(!result.evidence);
        assert(cursor_reads <= 2 && task_reads <= 3);
    }
};
} // namespace

int main()
{
    Fixture f;
    auto result = f.read();
    assert(result.status == Status::verified && result.evidence);
    assert(result.evidence->decision == Decision::idle);
    assert(result.evidence->cursor_revision == 3);
    assert(result.evidence->cursor_generation == 2);
    assert(result.evidence->predecessor_generation == 1);
    assert(!result.evidence->next_wake_after_ms);
    assert(result.evidence->result_sha256 == *f.cursor.predecessor_result_sha256);
    assert(f.cursor_reads == 2 && f.task_reads == 2);

    f.set_predecessor(succeeded(f.first, decision("request_openai")));
    result = f.read();
    assert(result.status == Status::verified && result.evidence);
    assert(result.evidence->decision == Decision::request_openai);

    for (const auto& bad : {decision("continue_local"), decision("idle", "1"),
                            decision("request_openai", "0"), decision("idle") + " ",
                            std::string("not-json"), std::string(16385, 'x')}) {
        Fixture x;
        x.set_predecessor(succeeded(x.first, bad));
        x.expect(Status::noncanonical);
    }
    { Fixture x; x.change = true; x.expect(Status::snapshot_changed); }
    { Fixture x; x.throw_read = true; x.expect(Status::unavailable); }
    { Fixture x; x.tasks.erase(x.first.id); x.expect(Status::unavailable); }
    { Fixture x; x.tasks.erase(x.anchor.id); x.expect(Status::unavailable); }
    { Fixture x; x.cursor.scope = "wrong"; x.expect(Status::noncanonical); assert(x.task_reads == 0); }
    { Fixture x; x.cursor.predecessor_result_sha256 = hex('0'); x.expect(Status::noncanonical); }
    { Fixture x; x.model = hex('f'); x.expect(Status::noncanonical); }
    { Fixture x; x.tasks.at(x.first.id).status = TaskStatus::failed; x.expect(Status::noncanonical); }
    { Fixture x; x.tasks.at(x.first.id).input += " "; x.expect(Status::noncanonical); }
    { Fixture x; x.tasks.at(x.anchor.id).result->output += " "; x.expect(Status::noncanonical); }
    { Fixture x; x.cursor.generation = 3; x.expect(Status::noncanonical); }
    { Fixture x; x.cursor.generation = 0; x.cursor.predecessor_task_id.reset();
      x.cursor.predecessor_result_sha256.reset(); x.expect(Status::no_predecessor); assert(x.task_reads == 0); }
    { Fixture x; x.cursor.state = LocalGooseCycleState::scheduled;
      x.cursor.due_at_ms = 20000; x.expect(Status::not_applicable); assert(x.task_reads == 0); }
    { Fixture x; x.cursor.state = LocalGooseCycleState::blocked;
      x.cursor.blocked_reason = "private"; x.expect(Status::not_applicable); assert(x.task_reads == 0); }

    // Immediate lineage (not a recursive history audit).
    Fixture recurring;
    const auto second = succeeded(make_local_goose_cycle_task(
        recurring.anchor, recurring.model, 2, 20000, 20001, recurring.first),
        decision("idle"));
    recurring.set_predecessor(second);
    recurring.cursor.generation = 3;
    recurring.cursor.revision = 6;
    result = recurring.read();
    assert(result.status == Status::verified && result.evidence);
    assert(result.evidence->predecessor_generation == 2);
    assert(recurring.task_reads == 3 && recurring.cursor_reads == 2);
    recurring.tasks.at(recurring.first.id).result->output = decision("request_openai");
    recurring.expect(Status::noncanonical);

    // Compare full snapshot, even when revision did not change.
    Fixture same_revision;
    int reads = 0;
    result = inspect_local_goose_settled_decision(
        [&]() -> std::optional<LocalGooseCycleCursor> {
            auto c = same_revision.cursor;
            if (++reads == 2) c.anchor_observation_result_sha256 = hex('f');
            return c;
        },
        [&](const std::string& id) -> std::optional<Task> {
            return same_revision.tasks.at(id);
        }, same_revision.model);
    assert(result.status == Status::snapshot_changed && !result.evidence);
    std::cout << "local Goose settled decision inspection: ok\n";
}
