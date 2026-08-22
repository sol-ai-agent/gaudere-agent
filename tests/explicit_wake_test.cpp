#include "BoundedReflection.hpp"
#include "ExplicitWake.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;
using WakeRuntime = gaudere::scheduling::wake::WakeIntentRuntime;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryDatabase {
    explicit TemporaryDatabase(std::string label = "explicit-wake")
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-agent-" + std::move(label) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + ".db");
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

std::string proposal(const std::uint64_t seconds,
                     const std::string& reason = "Revisit once.")
{
    return "{\"decision\":\"propose_wake\",\"reason\":\"" + reason
        + "\",\"schema\":\"gaudere.cognition.decision.v1\","
          "\"wake_after_seconds\":" + std::to_string(seconds) + "}";
}

gaudere::work::Task source_task(std::string id, std::string output)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "cognition.reflect.v1:" + task.id;
    task.kind = bounded_reflection_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = "bounded source fixture";
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 1s;
    task.limits.max_attempts = 2;
    task.attempts_started = 1;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        bounded_reflection_decision_content_type, std::move(output), {}, {}};
    return task;
}

gaudere::budget::Policy budget_policy()
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = 24h;
    policy.min_interval = 15min;
    return policy;
}

void test_fixed_capability_configuration()
{
    TemporaryDatabase database;
    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    auto now = gaudere::scheduling::wake::WakeIntentTimePoint{};

    WakeRuntime wrong_scope(wakes, [&now] { return now; }, "operator.scope", {1});
    try {
        static_cast<void>(ExplicitWake(tasks, wrong_scope));
        expect(false, "operator-selected wake scope is rejected");
    } catch (const std::invalid_argument&) {
        expect(true, "fixed wake scope mismatch fails explicitly");
    }

    WakeRuntime wrong_limit(
        wakes, [&now] { return now; }, explicit_wake_scope, {2});
    try {
        static_cast<void>(ExplicitWake(tasks, wrong_limit));
        expect(false, "raised wake lifetime limit is rejected");
    } catch (const std::invalid_argument&) {
        expect(true, "fixed one-wake lifetime mismatch fails explicitly");
    }
}

void test_accept_duplicate_revoke_and_budget_isolation()
{
    TemporaryDatabase database;
    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
    auto now = gaudere::scheduling::wake::WakeIntentTimePoint{1000s + 999us};
    WakeRuntime runtime(
        wakes, [&now] { return now; }, explicit_wake_scope,
        {explicit_wake_max_total});
    ExplicitWake capability(tasks, runtime);

    tasks.save(source_task("source-one", proposal(900)));
    const auto before = budgets.snapshot(
        "provider.call:openai.responses", now, budget_policy());
    const auto accepted = capability.accept("source-one");
    expect(accepted.result == ExplicitWakeAcceptResult::accepted
               && accepted.intent
               && accepted.intent->id == "source-one"
               && accepted.intent->source_id == "source-one"
               && accepted.intent->scope == explicit_wake_scope
               && accepted.intent->accepted_at
                    == gaudere::scheduling::wake::WakeIntentTimePoint{1000s}
               && accepted.intent->due_at
                    == gaudere::scheduling::wake::WakeIntentTimePoint{1900s},
           "canonical source accepts one exact app-scoped durable wake");
    expect(wake_intent_report(*accepted.intent).find("status=scheduled")
               != std::string::npos,
           "wake report exposes the complete scheduled durable state");

    now += 1min;
    const auto duplicate = capability.accept("source-one");
    expect(duplicate.result == ExplicitWakeAcceptResult::duplicate
               && duplicate.intent
               && duplicate.intent->due_at == accepted.intent->due_at,
           "repeated explicit acceptance preserves the original deadline");

    std::string invalid_utf8 = "operator";
    invalid_utf8.push_back(static_cast<char>(0xff));
    expect(capability.revoke("source-one", "bad\nreason")
               == gaudere::scheduling::wake::WakeIntentRevokeResult::invalid
               && capability.revoke("source-one", invalid_utf8)
                    == gaudere::scheduling::wake::WakeIntentRevokeResult::invalid
               && capability.find("source-one")->status
                    == gaudere::scheduling::wake::WakeIntentStatus::scheduled,
           "invalid revocation text cannot create unreportable durable state");

    now = gaudere::scheduling::wake::WakeIntentTimePoint{1100s};
    expect(capability.revoke("source-one", "operator revocation")
               == gaudere::scheduling::wake::WakeIntentRevokeResult::revoked,
           "operator revocation succeeds strictly before due");
    const auto revoked = capability.find("source-one");
    expect(revoked
               && revoked->status
                    == gaudere::scheduling::wake::WakeIntentStatus::revoked
               && revoked->terminal_reason == "operator revocation",
           "revocation is terminal and observable");

    tasks.save(source_task("source-two", proposal(900)));
    expect(capability.accept("source-two").result
               == ExplicitWakeAcceptResult::total_exhausted,
           "revocation never refunds the one-wake lifetime slot");
    const auto after = budgets.snapshot(
        "provider.call:openai.responses", now, budget_policy());
    expect(before.total_used == 0 && after.total_used == 0,
           "acceptance, duplicate, and revocation consume no provider permit");
}

void test_source_validation_fails_closed()
{
    TemporaryDatabase database;
    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    auto now = gaudere::scheduling::wake::WakeIntentTimePoint{2000s};
    WakeRuntime runtime(
        wakes, [&now] { return now; }, explicit_wake_scope,
        {explicit_wake_max_total});
    ExplicitWake capability(tasks, runtime);

    expect(capability.accept("missing").result
               == ExplicitWakeAcceptResult::source_not_found,
           "missing source is rejected without creating a wake");

    std::vector<gaudere::work::Task> invalid;
    auto wrong_kind = source_task("wrong-kind", proposal(900));
    wrong_kind.kind = "local.echo";
    invalid.push_back(wrong_kind);
    auto pending = source_task("pending", proposal(900));
    pending.status = gaudere::work::TaskStatus::pending;
    pending.attempts_started = 0;
    pending.result.reset();
    invalid.push_back(pending);
    auto wrong_type = source_task("wrong-type", proposal(900));
    wrong_type.result->content_type = "application/json";
    invalid.push_back(wrong_type);
    invalid.push_back(source_task(
        "stop",
        "{\"decision\":\"stop\",\"reason\":\"Done.\","
        "\"schema\":\"gaudere.cognition.decision.v1\"}"));
    invalid.push_back(source_task(
        "noncanonical",
        "{ \"decision\": \"propose_wake\", \"reason\": \"x\", "
        "\"schema\": \"gaudere.cognition.decision.v1\", "
        "\"wake_after_seconds\": 900 }"));
    invalid.push_back(source_task("too-soon", proposal(899)));
    invalid.push_back(source_task("too-late", proposal(86401)));
    invalid.push_back(source_task(
        "unknown-key",
        "{\"decision\":\"propose_wake\",\"extra\":true,"
        "\"reason\":\"x\",\"schema\":\"gaudere.cognition.decision.v1\","
        "\"wake_after_seconds\":900}"));

    for (const auto& task : invalid) {
        tasks.save(task);
        const auto result = capability.accept(task.id);
        expect(result.result == ExplicitWakeAcceptResult::source_ineligible
                   && !capability.find(task.id),
               "ineligible durable source shape creates no wake: " + task.id);
    }
}

void test_inclusive_maximum_delay_and_due_observation()
{
    TemporaryDatabase database("explicit-wake-maximum");
    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    gaudere::persistence::sqlite::WakeIntentStore wakes(database.path.string());
    auto now = gaudere::scheduling::wake::WakeIntentTimePoint{3000s};
    WakeRuntime runtime(
        wakes, [&now] { return now; }, explicit_wake_scope,
        {explicit_wake_max_total});
    ExplicitWake capability(tasks, runtime);

    tasks.save(source_task("maximum", proposal(86400)));
    const auto accepted = capability.accept("maximum");
    expect(accepted.result == ExplicitWakeAcceptResult::accepted
               && accepted.intent
               && accepted.intent->due_at == now + 86400s,
           "inclusive maximum proposal delay is accepted exactly");

    now += 86400s;
    const auto reconciled = runtime.reconcile();
    const auto fired = capability.find("maximum");
    expect(reconciled.fired == 1 && fired
               && fired->status
                    == gaudere::scheduling::wake::WakeIntentStatus::fired
               && fired->terminal_at == now,
           "due wake becomes observable fired state without successor work");
    expect(tasks.find("maximum")->status
               == gaudere::work::TaskStatus::succeeded,
           "firing does not alter or replace the source Task");
}

} // namespace

int main()
{
    test_fixed_capability_configuration();
    test_accept_duplicate_revoke_and_budget_isolation();
    test_source_validation_fails_closed();
    test_inclusive_maximum_delay_and_due_observation();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All explicit wake tests passed\n";
    return 0;
}
