#include "CurrentCognitionCycle.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

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
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;
using namespace gaudere_agent;
using namespace std::chrono_literals;

std::string decision_continue()
{
    return Json{
        {"schema", resume_after_wake_decision_schema},
        {"decision", "continue"},
        {"reason", "Canonical CI predecessor for provider-free pulse runtime gate."},
        {"objective", "Observe one bounded provider-free autonomous cognition pulse."}
    }.dump();
}

std::string snapshot_request(const std::string& content)
{
    return Json{
        {"schema", resume_context_snapshot_schema},
        {"content_type", "text/plain; charset=utf-8"},
        {"content", content},
        {"provenance", Json::array({Json{
            {"kind", "runtime-snapshot"},
            {"ref", "autonomous-pulse-runtime-gate-fixture"},
            {"sha256", sha256_hex(content)}
        }})}
    }.dump();
}

Task bootstrap_resume_task()
{
    Task task;
    task.id = "cognition.resume-after-wake.v0:autonomous-pulse-runtime-fixture";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "provider-free autonomous pulse runtime fixture";
    task.limits.max_input_bytes = 48 * 1024;
    task.limits.max_output_bytes = 8 * 1024;
    task.limits.max_runtime = 60s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        resume_after_wake_decision_content_type, decision_continue(), {}, {}};
    return task;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc != 2) throw std::invalid_argument("usage: fixture STATE_DB");
        const std::string state_path = argv[1];
        const auto now = [] { return std::chrono::system_clock::now(); };
        gaudere::persistence::sqlite::TaskStore tasks(state_path);
        gaudere::persistence::sqlite::ActionStore actions(state_path);
        gaudere::persistence::sqlite::BudgetStore budgets(state_path);
        gaudere::persistence::sqlite::WakeIntentStore wakes(state_path);
        (void)actions;
        (void)budgets;
        (void)wakes;
        gaudere::work::Runtime runtime(tasks, now);
        runtime.recover();

        const auto bootstrap = bootstrap_resume_task();
        tasks.save(bootstrap);

        ResumeContextSnapshotRecorder recorder(tasks, runtime, now);
        const auto content = std::string{
            "Canonical current cognition predecessor for runtime-gate integration proof."};
        const auto snapshot = recorder.record(snapshot_request(content));
        if ((snapshot.result != ResumeContextSnapshotRecordResult::accepted
             && snapshot.result != ResumeContextSnapshotRecordResult::duplicate)
            || !snapshot.task) {
            throw std::runtime_error("could not record fixture snapshot");
        }

        CurrentCognitionCycle cycle(tasks, runtime, now, true);
        const auto claim = cycle.claim(bootstrap.id, snapshot.task->id);
        if ((claim.result != CurrentCognitionClaimResult::accepted
             && claim.result != CurrentCognitionClaimResult::duplicate)
            || !claim.task) {
            throw std::runtime_error("could not claim fixture current cognition");
        }

        auto completed = *claim.task;
        completed.attempts_started = 1;
        completed.status = TaskStatus::succeeded;
        completed.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type, decision_continue(), {}, {}};
        tasks.save(completed);
        if (!valid_current_cognition_task(completed))
            throw std::runtime_error("fixture current cognition is non-canonical");

        std::cout << completed.id << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "autonomous pulse runtime fixture: " << error.what() << '\n';
        return 1;
    }
}
