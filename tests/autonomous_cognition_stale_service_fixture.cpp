#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "CurrentCognitionCycle.hpp"
#include "ResumeAfterWakeCognition.hpp"
#include "ResumeContextSnapshot.hpp"
#include "Sha256.hpp"

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
        {"reason", "Canonical stale-service fixture predecessor."},
        {"objective", "Prepare one pulse cognition that will be stale at real service startup."}
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
            {"ref", "autonomous-stale-service-fixture"},
            {"sha256", sha256_hex(content)}
        }})}
    }.dump();
}

Task bootstrap_resume_task()
{
    Task task;
    task.id = "cognition.resume-after-wake.v0:autonomous-stale-service-fixture";
    task.idempotency_key = task.id;
    task.kind = resume_after_wake_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "provider-free stale service fixture";
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
        if (argc != 3) {
            throw std::invalid_argument("usage: fixture STATE_DB PULSE_DB");
        }
        const std::string state_path = argv[1];
        const std::string pulse_path = argv[2];

        auto fixture_now = std::chrono::system_clock::now()
            - autonomous_cognition_continue_cadence
            - current_cognition_max_snapshot_age - 1min;
        const auto now = [&fixture_now] { return fixture_now; };

        gaudere::persistence::sqlite::TaskStore tasks(state_path);
        gaudere::persistence::sqlite::BudgetStore budgets(state_path);
        gaudere::persistence::sqlite::WakeIntentStore wakes(state_path);
        gaudere::work::Runtime runtime(tasks, now);
        runtime.recover();

        const auto bootstrap = bootstrap_resume_task();
        tasks.save(bootstrap);

        ResumeContextSnapshotRecorder recorder(tasks, runtime, now);
        const auto predecessor_snapshot = recorder.record(snapshot_request(
            "Canonical predecessor facts for stale service wiring proof."));
        if ((predecessor_snapshot.result != ResumeContextSnapshotRecordResult::accepted
             && predecessor_snapshot.result != ResumeContextSnapshotRecordResult::duplicate)
            || !predecessor_snapshot.task) {
            throw std::runtime_error("could not record predecessor snapshot");
        }

        CurrentCognitionCycle cycle(tasks, runtime, now, true);
        const auto predecessor_claim = cycle.claim(
            bootstrap.id, predecessor_snapshot.task->id);
        if ((predecessor_claim.result != CurrentCognitionClaimResult::accepted
             && predecessor_claim.result != CurrentCognitionClaimResult::duplicate)
            || !predecessor_claim.task) {
            throw std::runtime_error("could not claim predecessor cognition");
        }
        auto predecessor = *predecessor_claim.task;
        predecessor.attempts_started = 1;
        predecessor.status = TaskStatus::succeeded;
        predecessor.result = gaudere::work::TaskResult{
            resume_after_wake_decision_content_type, decision_continue(), {}, {}};
        tasks.save(predecessor);
        if (!valid_current_cognition_task(predecessor)) {
            throw std::runtime_error("predecessor cognition is non-canonical");
        }

        AutonomousCognitionPulseStore pulse_store(pulse_path);
        AutonomousCognitionPulse pulse(
            pulse_store, tasks, budgets, wakes, runtime, now, true);
        const auto seeded = pulse.seed(predecessor.id);
        if (seeded.result != AutonomousCognitionPulseResult::seeded
            && seeded.result != AutonomousCognitionPulseResult::duplicate) {
            throw std::runtime_error("could not seed stale-service pulse");
        }

        fixture_now += autonomous_cognition_continue_cadence;
        const auto prepared = pulse.observe();
        if (prepared.result != AutonomousCognitionPulseResult::prepared
            || !prepared.cursor || !prepared.task) {
            throw std::runtime_error("could not prepare stale pulse cognition");
        }
        if (prepared.task->status != TaskStatus::pending
            || prepared.task->attempts_started != 0) {
            throw std::runtime_error("prepared stale cognition has execution evidence");
        }

        std::cout << "predecessor_id=" << predecessor.id << '\n'
                  << "stale_task_id=" << prepared.task->id << '\n'
                  << "generation=" << prepared.cursor->generation << '\n'
                  << "captured_at_ms=" << *prepared.cursor->observed_at_ms << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "autonomous stale service fixture: " << error.what() << '\n';
        return 1;
    }
}
