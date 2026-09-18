#include "LocalGooseCycleBootstrap.hpp"
#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using TimePoint = gaudere::work::TimePoint;

std::string hex(const char c) { return std::string(64, c); }

Task observation(const std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1'000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 1;
    facts.anchor_checkpoint_task_id = "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('b');
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
        "provider.call:openai.responses:cognition.current.v0:" + hex('c');
    facts.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('d');
    facts.historical_wake_scope = "cognition.reflect.wake.v0";
    facts.historical_wake_sha256 = hex('e');

    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{
        local_continuity_observation_content_type, task.input, {}, {}};
    assert(canonical_local_continuity_observation_success(task));
    return task;
}

class MemoryTaskStore final : public gaudere::work::TaskStore {
public:
    std::optional<Task> find(const std::string& id) const override
    {
        const auto it = tasks.find(id);
        return it == tasks.end() ? std::nullopt
                                 : std::optional<Task>{it->second};
    }

    std::optional<Task> find_by_idempotency_key(
        const std::string& key) const override
    {
        for (const auto& item : tasks)
            if (item.second.idempotency_key == key) return item.second;
        return std::nullopt;
    }

    std::optional<Task> find_pending_for(
        const std::vector<std::string>& accepted_kinds) const override
    {
        for (const auto& item : tasks) {
            if (item.second.status == TaskStatus::pending
                && std::find(accepted_kinds.begin(), accepted_kinds.end(),
                             item.second.kind) != accepted_kinds.end())
                return item.second;
        }
        return std::nullopt;
    }

    std::vector<Task> leased_with_expired_lease(TimePoint) const override
    {
        return {};
    }

    std::optional<TimePoint> next_lease_expiry() const override
    {
        return std::nullopt;
    }

    bool has_active() const override { return false; }

    void save(const Task& task) override { tasks[task.id] = task; }

    std::map<std::string, Task> tasks;
};

std::string temp_path()
{
    return std::string{"/tmp/gaudere-local-goose-cycle-bootstrap-"}
        + std::to_string(static_cast<long long>(getpid())) + ".db";
}

} // namespace

int main()
{
    const auto first = observation(1);
    const auto second = observation(2, first);
    const auto anchor = observation(3, second);

    MemoryTaskStore tasks;
    tasks.save(anchor);

    LocalActivityPulseCursor activity;
    activity.revision = 5;
    activity.generation = 3;
    activity.state = LocalActivityPulseState::quiescent;
    activity.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + hex('a');
    activity.anchor_checkpoint_result_sha256 = hex('b');
    activity.anchor_at_ms = 1'000;
    activity.due_at_ms = 3'000;
    activity.captured_at_ms = 3'001;
    activity.task_id = anchor.id;
    activity.result_sha256 = sha256_hex(anchor.result->output);
    activity.predecessor_observation_task_id = second.id;
    activity.predecessor_observation_result_sha256 =
        sha256_hex(second.result->output);
    assert(valid_local_activity_pulse_cursor(activity));

    const auto path = temp_path();
    std::remove(path.c_str());
    LocalGooseCycleStore store(path);

    const auto seeded = seed_local_goose_cycle(store, tasks, activity);
    assert(seeded.result == LocalGooseCycleBootstrapResult::seeded);
    assert(seeded.cursor);
    assert(seeded.cursor->state == LocalGooseCycleState::dormant);
    assert(seeded.cursor->generation == 0);
    assert(seeded.cursor->anchor_observation_task_id == anchor.id);
    assert(seeded.cursor->anchor_observation_result_sha256
           == sha256_hex(anchor.result->output));

    const auto duplicate = seed_local_goose_cycle(store, tasks, activity);
    assert(duplicate.result == LocalGooseCycleBootstrapResult::duplicate);

    const auto activated = activate_local_goose_cycle(store, 12'345);
    assert(activated.result == LocalGooseCycleBootstrapResult::activated);
    assert(activated.cursor);
    assert(activated.cursor->state == LocalGooseCycleState::scheduled);
    assert(activated.cursor->generation == 1);
    assert(activated.cursor->revision == 1);
    assert(activated.cursor->due_at_ms == 12'345);

    const auto already = activate_local_goose_cycle(store, 99'999);
    assert(already.result == LocalGooseCycleBootstrapResult::already_active);
    assert(already.cursor && already.cursor->due_at_ms == 12'345);

    auto invalid_activity = activity;
    invalid_activity.state = LocalActivityPulseState::settled;
    const auto invalid = seed_local_goose_cycle(store, tasks, invalid_activity);
    assert(invalid.result == LocalGooseCycleBootstrapResult::invalid);

    std::remove(path.c_str());
    std::cout << "local Goose cycle bootstrap: ok\n";
    return 0;
}
