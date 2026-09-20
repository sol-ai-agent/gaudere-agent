#include "LocalGooseCycle.hpp"
#include "LocalGooseCycleStimulusControl.hpp"
#include "LocalGooseCycleStimulusService.hpp"
#include "LocalGooseCycleStimulusStore.hpp"
#include "LocalGooseCycleStore.hpp"
#include "Sha256.hpp"

#include <gaudere/work/TaskStore.hpp>

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Json = nlohmann::json;
using gaudere::work::Task;
using gaudere::work::TaskStatus;
using gaudere_agent::LocalGooseCycleCursor;
using gaudere_agent::LocalGooseCycleState;
using gaudere_agent::LocalGooseCycleStimulusService;
using gaudere_agent::LocalGooseCycleStimulusServiceResult;
using gaudere_agent::LocalGooseCycleStimulusStatus;
using gaudere_agent::LocalGooseCycleStimulusStore;
using gaudere_agent::LocalGooseCycleStore;

class FakeTaskStore final : public gaudere::work::TaskStore {
public:
    std::optional<Task> find(const std::string& id) const override
    {
        const auto found = tasks.find(id);
        return found == tasks.end()
            ? std::nullopt : std::optional<Task>{found->second};
    }

    std::optional<Task> find_by_idempotency_key(
        const std::string& key) const override
    {
        for (const auto& item : tasks) {
            if (item.second.idempotency_key == key) return item.second;
        }
        return std::nullopt;
    }

    std::optional<Task> find_pending_for(
        const std::vector<std::string>&) const override
    {
        return std::nullopt;
    }

    std::vector<Task> leased_with_expired_lease(
        gaudere::work::TimePoint) const override
    {
        return {};
    }

    std::optional<gaudere::work::TimePoint>
    next_lease_expiry() const override
    {
        return std::nullopt;
    }

    bool has_active() const override { return false; }

    void save(const Task& task) override { tasks[task.id] = task; }

    std::unordered_map<std::string, Task> tasks;
};

std::string temporary_path(const char* label)
{
    static std::uint64_t sequence = 0;
    const auto now = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return (std::filesystem::temp_directory_path()
        / (std::string{"gaudere-"} + label + "-"
           + std::to_string(static_cast<long long>(now))
           + "-" + std::to_string(++sequence) + ".db")).string();
}

void remove_if_present(const std::string& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

Task canonical_generation_one_predecessor()
{
    const std::string anchor_task =
        std::string{"continuity.local-observation.v1:"}
        + std::string(64, 'a');
    const std::string anchor_hash(64, 'b');
    const std::string model_hash(64, 'c');

    const Json input{
        {"anchor_observation_result_sha256", anchor_hash},
        {"anchor_observation_task_id", anchor_task},
        {"captured_at_ms", 10},
        {"due_at_ms", 10},
        {"generation", 1},
        {"model_sha256", model_hash},
        {"predecessor", nullptr},
        {"schema", gaudere_agent::local_goose_cycle_schema}
    };

    Task task;
    task.input = input.dump();
    const auto identity = gaudere_agent::sha256_hex(task.input);
    task.id = std::string{gaudere_agent::local_goose_cycle_task_prefix}
        + identity;
    task.idempotency_key =
        std::string{gaudere_agent::local_goose_cycle_task_prefix}
        + "input:" + identity;
    task.kind = gaudere_agent::local_goose_cycle_task_kind;
    task.input_content_type =
        gaudere_agent::local_goose_cycle_content_type;
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 16 * 1024;
    task.limits.max_runtime = std::chrono::minutes{10};
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;

    const Json decision{
        {"assessment", "canonical predecessor"},
        {"decision", "idle"},
        {"next_wake_after_ms", nullptr},
        {"openai_request", nullptr},
        {"reason", "test predecessor settled dormant"},
        {"schema", gaudere_agent::local_goose_decision_schema}
    };
    task.result = gaudere::work::TaskResult{
        gaudere_agent::local_goose_decision_content_type,
        decision.dump(), {}, {}};

    assert(gaudere_agent::canonical_local_goose_cycle_success(task));
    return task;
}

LocalGooseCycleCursor make_dormant_generation_two(
    LocalGooseCycleStore& store,
    const Task& predecessor)
{
    const auto inspected =
        gaudere_agent::inspect_local_goose_cycle_task(predecessor);
    assert(inspected.eligible);
    assert(predecessor.result);

    LocalGooseCycleCursor seed;
    seed.anchor_observation_task_id =
        inspected.anchor_observation_task_id;
    seed.anchor_observation_result_sha256 =
        inspected.anchor_observation_result_sha256;
    assert(store.seed(seed).result
        == gaudere_agent::LocalGooseCycleStoreResult::accepted);

    auto scheduled = seed;
    scheduled.revision = 1;
    scheduled.generation = 1;
    scheduled.state = LocalGooseCycleState::scheduled;
    scheduled.due_at_ms = 10;
    assert(store.replace(seed, scheduled).result
        == gaudere_agent::LocalGooseCycleStoreResult::accepted);

    auto prepared = scheduled;
    prepared.revision = 2;
    prepared.state = LocalGooseCycleState::prepared;
    prepared.captured_at_ms = 10;
    prepared.current_task_id = predecessor.id;
    assert(store.replace(scheduled, prepared).result
        == gaudere_agent::LocalGooseCycleStoreResult::accepted);

    auto dormant = prepared;
    dormant.revision = 3;
    dormant.generation = 2;
    dormant.state = LocalGooseCycleState::dormant;
    dormant.predecessor_task_id = predecessor.id;
    dormant.predecessor_result_sha256 =
        gaudere_agent::sha256_hex(predecessor.result->output);
    dormant.due_at_ms.reset();
    dormant.captured_at_ms.reset();
    dormant.current_task_id.clear();
    assert(store.replace(prepared, dormant).result
        == gaudere_agent::LocalGooseCycleStoreResult::accepted);

    return dormant;
}

void test_accept_and_consume()
{
    const auto cycle_path = temporary_path("cycle-rearm");
    const auto stimulus_path = temporary_path("stimulus-rearm");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks.save(predecessor);

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        const auto dormant =
            make_dormant_generation_two(cycle_store, predecessor);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, tasks);

        const auto accepted =
            service.accept_explicit_recheck("recheck-live-001", 1000);
        assert(accepted.result
            == LocalGooseCycleStimulusServiceResult::accepted);
        assert(accepted.stimulus);
        assert(accepted.stimulus->target_cycle_revision
            == dormant.revision);

        const auto duplicate =
            service.accept_explicit_recheck("recheck-live-001", 1001);
        assert(duplicate.result
            == LocalGooseCycleStimulusServiceResult::duplicate);
        assert(duplicate.stimulus
            && duplicate.stimulus->id == accepted.stimulus->id);

        const auto consumed =
            service.reconcile(accepted.stimulus->id, 1002);
        assert(consumed.result
            == LocalGooseCycleStimulusServiceResult::consumed);
        assert(consumed.stimulus
            && consumed.stimulus->status
                == LocalGooseCycleStimulusStatus::consumed);
        assert(consumed.cursor
            && consumed.cursor->revision == dormant.revision + 1
            && consumed.cursor->generation == dormant.generation
            && consumed.cursor->state == LocalGooseCycleState::scheduled
            && consumed.cursor->due_at_ms
                == std::optional<std::int64_t>{1000});

        const auto again =
            service.reconcile(accepted.stimulus->id, 1003);
        assert(again.result
            == LocalGooseCycleStimulusServiceResult::consumed);
        assert(again.cursor && again.cursor->revision == 4);
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

void test_restart_after_cycle_cas_before_stimulus_terminal()
{
    const auto cycle_path = temporary_path("cycle-crash");
    const auto stimulus_path = temporary_path("stimulus-crash");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks.save(predecessor);

    std::string stimulus_id;
    {
        LocalGooseCycleStore cycle_store(cycle_path);
        auto dormant =
            make_dormant_generation_two(cycle_store, predecessor);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, tasks);

        const auto accepted =
            service.accept_explicit_recheck("recheck-crash-001", 2000);
        assert(accepted.result
            == LocalGooseCycleStimulusServiceResult::accepted);
        assert(accepted.stimulus);
        stimulus_id = accepted.stimulus->id;

        auto scheduled = dormant;
        ++scheduled.revision;
        scheduled.state = LocalGooseCycleState::scheduled;
        scheduled.due_at_ms = accepted.stimulus->accepted_at_ms;
        assert(cycle_store.replace(dormant, scheduled).result
            == gaudere_agent::LocalGooseCycleStoreResult::accepted);
        // Simulated crash here: stimulus remains accepted.
    }

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService recovered(
            stimulus_store, cycle_store, tasks);

        const auto step = recovered.reconcile(stimulus_id, 2001);
        assert(step.result
            == LocalGooseCycleStimulusServiceResult::consumed);
        assert(step.cursor && step.cursor->revision == 4);
        const auto stored = stimulus_store.find(stimulus_id);
        assert(stored
            && stored->status == LocalGooseCycleStimulusStatus::consumed
            && stored->resulting_cycle_revision
                == std::optional<std::uint64_t>{4});
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

void test_ambiguous_successor_requires_manual_review()
{
    const auto cycle_path = temporary_path("cycle-ambiguous");
    const auto stimulus_path = temporary_path("stimulus-ambiguous");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks.save(predecessor);

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        auto dormant =
            make_dormant_generation_two(cycle_store, predecessor);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, tasks);

        const auto accepted =
            service.accept_explicit_recheck("recheck-ambiguous-001", 2500);
        assert(accepted.stimulus);

        auto ambiguous = dormant;
        ++ambiguous.revision;
        ambiguous.state = LocalGooseCycleState::scheduled;
        ambiguous.due_at_ms = 9999;
        assert(cycle_store.replace(dormant, ambiguous).result
            == gaudere_agent::LocalGooseCycleStoreResult::accepted);

        const auto step =
            service.reconcile(accepted.stimulus->id, 2501);
        assert(step.result
            == LocalGooseCycleStimulusServiceResult::manual_review);
        assert(step.stimulus
            && step.stimulus->status
                == LocalGooseCycleStimulusStatus::manual_review);
        const auto current =
            cycle_store.find(gaudere_agent::local_goose_cycle_scope);
        assert(current && current->revision == 4
            && current->due_at_ms
                == std::optional<std::int64_t>{9999});
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

void test_changed_cursor_is_superseded()
{
    const auto cycle_path = temporary_path("cycle-superseded");
    const auto stimulus_path = temporary_path("stimulus-superseded");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks.save(predecessor);

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        auto dormant =
            make_dormant_generation_two(cycle_store, predecessor);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, tasks);

        const auto accepted =
            service.accept_explicit_recheck("recheck-stale-001", 3000);
        assert(accepted.stimulus);

        auto different = dormant;
        ++different.revision;
        different.state = LocalGooseCycleState::scheduled;
        different.due_at_ms = 9999;
        assert(cycle_store.replace(dormant, different).result
            == gaudere_agent::LocalGooseCycleStoreResult::accepted);

        const auto step =
            service.reconcile(accepted.stimulus->id, 3001);
        assert(step.result
            == LocalGooseCycleStimulusServiceResult::superseded);
        assert(step.stimulus
            && step.stimulus->status
                == LocalGooseCycleStimulusStatus::superseded);
        const auto current =
            cycle_store.find(gaudere_agent::local_goose_cycle_scope);
        assert(current && current->revision == 4
            && current->due_at_ms
                == std::optional<std::int64_t>{9999});
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

void test_live_control_adapter_consumes_one_bounded_recheck()
{
    const auto cycle_path = temporary_path("cycle-control");
    const auto stimulus_path = temporary_path("stimulus-control");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks.save(predecessor);

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        const auto dormant =
            make_dormant_generation_two(cycle_store, predecessor);
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, tasks);
        gaudere_agent::LocalGooseCycleStimulusControl control(
            service, [] { return std::int64_t{5000}; });

        const auto reply = control.stimulate("control-recheck-001");
        assert(reply.ok && reply.code == 0);
        assert(reply.body.find("acceptance=accepted") != std::string::npos);
        assert(reply.body.find("result=consumed") != std::string::npos);
        assert(reply.body.find("cycle_state=scheduled") != std::string::npos);

        const auto cursor =
            cycle_store.find(gaudere_agent::local_goose_cycle_scope);
        assert(cursor
            && cursor->revision == dormant.revision + 1
            && cursor->generation == dormant.generation
            && cursor->state == LocalGooseCycleState::scheduled
            && cursor->due_at_ms == std::optional<std::int64_t>{5000});

        const auto retry = control.stimulate("control-recheck-001");
        assert(retry.ok && retry.code == 0);
        assert(retry.body.find("acceptance=duplicate") != std::string::npos);
        assert(retry.body.find("result=consumed") != std::string::npos);
        const auto unchanged =
            cycle_store.find(gaudere_agent::local_goose_cycle_scope);
        assert(unchanged && unchanged->revision == dormant.revision + 1);
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

void test_missing_predecessor_is_rejected_without_stimulus()
{
    const auto cycle_path = temporary_path("cycle-missing-pred");
    const auto stimulus_path = temporary_path("stimulus-missing-pred");
    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);

    FakeTaskStore tasks_for_setup;
    const auto predecessor = canonical_generation_one_predecessor();
    tasks_for_setup.save(predecessor);

    {
        LocalGooseCycleStore cycle_store(cycle_path);
        static_cast<void>(
            make_dormant_generation_two(cycle_store, predecessor));
        LocalGooseCycleStimulusStore stimulus_store(stimulus_path);
        FakeTaskStore empty_tasks;
        LocalGooseCycleStimulusService service(
            stimulus_store, cycle_store, empty_tasks);

        const auto step =
            service.accept_explicit_recheck("recheck-no-pred", 4000);
        assert(step.result
            == LocalGooseCycleStimulusServiceResult::invalid);
        assert(!step.stimulus);
        assert(!stimulus_store.find_by_source("recheck-no-pred"));
    }

    remove_if_present(cycle_path);
    remove_if_present(stimulus_path);
}

} // namespace

int main()
{
    test_accept_and_consume();
    test_restart_after_cycle_cas_before_stimulus_terminal();
    test_ambiguous_successor_requires_manual_review();
    test_changed_cursor_is_superseded();
    test_live_control_adapter_consumes_one_bounded_recheck();
    test_missing_predecessor_is_rejected_without_stimulus();
    std::cout << "local_goose_cycle_stimulus_service_test: PASS\n";
    return 0;
}
