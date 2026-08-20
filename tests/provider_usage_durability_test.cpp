#include "ProviderTaskHandler.hpp"
#include "TaskExecutor.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;

struct TemporaryDatabase {
    TemporaryDatabase()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-provider-usage-test-" + std::to_string(
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

class UsageProvider final : public Provider {
public:
    std::string_view name() const noexcept override { return "usage-test"; }

    ProviderResult invoke(const ProviderRequest&) override
    {
        return ProviderResult{
            ProviderOutcome::succeeded,
            "text/plain; charset=utf-8",
            "provider answer",
            {}, {},
            "application/vnd.gaudere.provider-usage+json",
            R"({"schema":"gaudere.provider_usage.v1","provider":"openai","model":"gpt-test","input_tokens":7,"cached_input_tokens":2,"cache_write_input_tokens":0,"output_tokens":5,"reasoning_tokens":3,"total_tokens":12})"};
    }
};

gaudere::work::Task make_task()
{
    gaudere::work::Task task;
    task.id = "usage-durable";
    task.idempotency_key = "usage-durable-key";
    task.kind = "provider.usage-test";
    task.input_content_type = "text/plain";
    task.input = "hello";
    task.limits.max_input_bytes = 1024;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = 2s;
    task.limits.max_attempts = 2;
    return task;
}

gaudere::budget::Policy policy()
{
    gaudere::budget::Policy value;
    value.max_total = 12;
    value.max_in_window = 4;
    value.window = 24h;
    value.min_interval = 15min;
    return value;
}

} // namespace

int main()
{
    TemporaryDatabase database;
    {
        gaudere::persistence::sqlite::ActionStore actions(database.path.string());
        gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
        gaudere::persistence::sqlite::BudgetStore budgets(database.path.string());
        gaudere::scheduling::wake::Runtime action_runtime(
            actions, [] { return gaudere::scheduling::wake::TimePoint{}; });
        gaudere::work::Runtime work_runtime(
            tasks, [] { return gaudere::work::TimePoint{}; });
        action_runtime.recover();
        work_runtime.recover();

        UsageProvider provider;
        const auto budget_policy = policy();
        ProviderTaskHandler handler(
            action_runtime, actions, provider, budgets, budget_policy,
            [] { return gaudere::budget::TimePoint{}; });
        TaskExecutor executor(work_runtime, tasks);

        const auto task = make_task();
        if (work_runtime.submit(task) != gaudere::work::SubmitResult::accepted
            || executor.execute(task.id, "usage-worker", handler)
                != ExecuteResult::completed) {
            std::cerr << "provider usage task did not execute\n";
            return 1;
        }
        const auto done = tasks.find(task.id);
        if (!done || !done->result
            || done->result->metadata_content_type
                != "application/vnd.gaudere.provider-usage+json"
            || done->result->metadata.find("\"total_tokens\":12")
                == std::string::npos) {
            std::cerr << "provider metadata did not reach the durable task result\n";
            return 1;
        }
    }

    gaudere::persistence::sqlite::TaskStore reopened(database.path.string());
    const auto durable = reopened.find("usage-durable");
    if (!durable || !durable->result
        || durable->result->output != "provider answer"
        || durable->result->metadata_content_type
            != "application/vnd.gaudere.provider-usage+json"
        || durable->result->metadata.find("\"input_tokens\":7")
            == std::string::npos
        || durable->result->metadata.find("\"total_tokens\":12")
            == std::string::npos) {
        std::cerr << "provider usage metadata did not survive SQLite reopen\n";
        return 1;
    }

    std::cout << "All provider usage durability tests passed\n";
    return 0;
}
