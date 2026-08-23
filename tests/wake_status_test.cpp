#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"
#include "LiveControlProcessor.hpp"
#include "OpenAIActivation.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace gaudere_agent;
using WakeTime = gaudere::scheduling::wake::WakeIntentTimePoint;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-wake-status-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    }

    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

gaudere::work::Task reflection_source(const std::string& id)
{
    gaudere::work::Task task;
    task.id = id;
    task.idempotency_key = "cognition.reflect.v1:" + id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "wake status source fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type,
        "{\"decision\":\"propose_wake\",\"reason\":\"Observe once.\","
        "\"schema\":\"gaudere.cognition.decision.v1\","
        "\"wake_after_seconds\":900}", {}, {}};
    return task;
}

struct Harness {
    explicit Harness(const std::filesystem::path& path,
                     const bool wake_enabled = true)
        : now(WakeTime{100s}),
          task_store(path.string()),
          budget_store(path.string()),
          wake_store(path.string()),
          runtime(task_store, [this] { return now; }),
          wake_runtime(wake_store, [this] { return now; },
                       explicit_wake_scope, {explicit_wake_max_total}),
          explicit_wake(task_store, wake_runtime),
          processor(runtime, task_store, budget_store,
                    OpenAIActivation::bootstrap_budget_policy(), false,
                    wake_enabled ? &explicit_wake : nullptr,
                    [this] { return scheduler_next; })
    {
        runtime.recover();
    }

    LiveControlReply status()
    {
        auto pending = mailbox.submit(
            LiveControlCommand{LiveControlOperation::inspect_wake_status,
                               "current", {}});
        const auto processed = processor.process(mailbox);
        expect(processed.processed == 1
                   && !processed.work_may_be_pending
                   && !processed.wake_deadline_may_have_changed,
               "wake-status is observational and requests no transition");
        return pending->wait();
    }

    WakeTime now;
    std::optional<WakeTime> scheduler_next;
    gaudere::persistence::sqlite::TaskStore task_store;
    gaudere::persistence::sqlite::BudgetStore budget_store;
    gaudere::persistence::sqlite::WakeIntentStore wake_store;
    gaudere::work::Runtime runtime;
    gaudere::scheduling::wake::WakeIntentRuntime wake_runtime;
    ExplicitWake explicit_wake;
    LiveControlProcessor processor;
    LiveControlMailbox mailbox;
};

void test_disabled_and_empty()
{
    TemporaryDatabase database;
    Harness disabled(database.path, false);
    const auto denied = disabled.status();
    expect(!denied.ok && denied.code == 4
               && denied.body.find("not enabled") != std::string::npos,
           "wake-status refuses when explicit capability is disabled");

    TemporaryDatabase empty_database;
    Harness empty(empty_database.path);
    empty.scheduler_next = empty.now;
    const auto report = empty.status();
    expect(report.ok && report.code == 0
               && report.body.find("report_schema=\"gaudere.wake_status.v1\"")
                    != std::string::npos
               && report.body.find("record=none") != std::string::npos
               && report.body.find("health=empty") != std::string::npos
               && report.body.find("scheduler_coverage=not_applicable")
                    != std::string::npos,
           "empty enabled scope reports healthy empty state");
}

void test_exact_and_earlier_scheduler_coverage()
{
    TemporaryDatabase database;
    Harness harness(database.path);
    harness.task_store.save(reflection_source("source"));
    const auto accepted = harness.explicit_wake.accept("source");
    expect(accepted.result == ExplicitWakeAcceptResult::accepted
               && accepted.intent,
           "wake status fixture accepts canonical source");
    const auto due = accepted.intent->due_at;

    harness.scheduler_next = due;
    const auto exact = harness.status();
    expect(exact.ok
               && exact.body.find("record=one") != std::string::npos
               && exact.body.find("source_consistency=eligible")
                    != std::string::npos
               && exact.body.find("source_task_status=succeeded")
                    != std::string::npos
               && exact.body.find("source_task_attempts=1/2")
                    != std::string::npos
               && exact.body.find("health=ok") != std::string::npos
               && exact.body.find("scheduler_coverage=exact")
                    != std::string::npos
               && exact.body.find("Observe once") == std::string::npos,
           "exact scheduler coverage is healthy without exposing provider output");

    harness.scheduler_next = due - 1s;
    const auto earlier = harness.status();
    expect(earlier.ok
               && earlier.body.find("scheduler_coverage=covered_by_earlier_event")
                    != std::string::npos,
           "earlier scheduler event safely covers a later wake deadline");
}

void test_missing_late_and_source_inconsistency_fail_closed()
{
    TemporaryDatabase missing_database;
    Harness missing(missing_database.path);
    missing.task_store.save(reflection_source("source"));
    const auto accepted = missing.explicit_wake.accept("source");
    expect(accepted.intent.has_value(), "missing scheduler fixture accepted");
    missing.scheduler_next.reset();
    const auto no_schedule = missing.status();
    expect(!no_schedule.ok && no_schedule.code == 4
               && no_schedule.body.find("health=scheduling_divergence")
                    != std::string::npos
               && no_schedule.body.find("scheduler_coverage=missing")
                    != std::string::npos,
           "missing scheduler deadline fails closed");

    missing.scheduler_next = accepted.intent->due_at + 1ms;
    const auto late = missing.status();
    expect(!late.ok && late.code == 4
               && late.body.find("scheduler_coverage=late")
                    != std::string::npos,
           "scheduler deadline after durable wake fails closed");

    TemporaryDatabase source_database;
    Harness source(source_database.path);
    gaudere::scheduling::wake::WakeIntent orphan;
    orphan.scope = explicit_wake_scope;
    orphan.id = "orphan";
    orphan.source_id = "missing-source";
    orphan.accepted_at = source.now;
    orphan.due_at = source.now + 900s;
    expect(source.wake_store.accept(orphan, {1})
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "orphan fixture inserted through durable store contract");
    source.scheduler_next = orphan.due_at;
    const auto inconsistent = source.status();
    expect(!inconsistent.ok && inconsistent.code == 4
               && inconsistent.body.find("source_consistency=missing")
                    != std::string::npos
               && inconsistent.body.find("health=source_inconsistent")
                    != std::string::npos,
           "missing source task fails closed");
}

void test_ambiguity_and_terminal_state()
{
    TemporaryDatabase ambiguous_database;
    Harness ambiguous(ambiguous_database.path);
    gaudere::scheduling::wake::WakeIntent first;
    first.scope = explicit_wake_scope;
    first.id = "a";
    first.source_id = "source-a";
    first.accepted_at = ambiguous.now;
    first.due_at = ambiguous.now + 900s;
    auto second = first;
    second.id = "b";
    second.source_id = "source-b";
    expect(ambiguous.wake_store.accept(first, {2})
               == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted
               && ambiguous.wake_store.accept(second, {2})
                    == gaudere::scheduling::wake::WakeIntentAcceptResult::accepted,
           "ambiguity fixture stores two records in fixed scope");
    ambiguous.scheduler_next = first.due_at;
    const auto ambiguous_report = ambiguous.status();
    expect(!ambiguous_report.ok && ambiguous_report.code == 4
               && ambiguous_report.body.find("record=ambiguous")
                    != std::string::npos
               && ambiguous_report.body.find("health=ambiguous")
                    != std::string::npos
               && ambiguous_report.body.find("id=\"") == std::string::npos,
           "ambiguous scope exposes no arbitrary identity and fails closed");

    TemporaryDatabase terminal_database;
    Harness terminal(terminal_database.path);
    terminal.task_store.save(reflection_source("source"));
    const auto accepted = terminal.explicit_wake.accept("source");
    terminal.now = accepted.intent->due_at;
    const auto reconciled = terminal.wake_runtime.reconcile();
    expect(reconciled.fired == 1, "terminal fixture fires durably");
    terminal.scheduler_next.reset();
    const auto report = terminal.status();
    expect(report.ok && report.code == 0
               && report.body.find("status=fired") != std::string::npos
               && report.body.find("health=terminal") != std::string::npos
               && report.body.find("scheduler_coverage=not_applicable")
                    != std::string::npos,
           "terminal fired wake is healthy without requiring an armed scheduler");
}

} // namespace

int main()
{
    test_disabled_and_empty();
    test_exact_and_earlier_scheduler_coverage();
    test_missing_late_and_source_inconsistency_fail_closed();
    test_ambiguity_and_terminal_state();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All wake status tests passed\n";
    return 0;
}
