#include "LiveControlProcessor.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

using namespace gaudere_agent;

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
            / ("gaudere-live-control-processor-test-" + std::to_string(
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

struct Harness {
    Harness(const std::filesystem::path& path, const bool openai_enabled)
        : store(path.string()),
          runtime(store, [] { return std::chrono::system_clock::now(); }),
          processor(runtime, store, openai_enabled)
    {
        runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore store;
    gaudere::work::Runtime runtime;
    LiveControlProcessor processor;
    LiveControlMailbox mailbox;
};

void test_echo_submission_is_durable()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "echo-live", "bonjour"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();
    const auto task = harness.store.find("echo-live");

    expect(processed.processed == 1 && processed.work_may_be_pending,
           "echo submission asks the worker loop to dispatch pending work");
    expect(reply.ok && reply.code == 0,
           "echo submission receives successful durable acknowledgement");
    expect(reply.body.find("status=pending") != std::string::npos,
           "echo reply is a durable pending Task report");
    expect(task && task->kind == "local.echo" && task->input == "bonjour",
           "echo command is persisted through Runtime on worker side");
    expect(task && task->limits.max_attempts == 1,
           "live echo uses the same bounded local task policy");
}

void test_openai_submission_requires_activated_provider()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_openai,
                           "ai-disabled", "bonjour"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "disabled provider does not create dispatchable work");
    expect(!reply.ok && reply.code == 4
               && reply.body.find("not enabled") != std::string::npos,
           "OpenAI submission is explicitly rejected when service is offline-provider mode");
    expect(!harness.store.find("ai-disabled"),
           "disabled provider command creates no durable Task");
}

void test_openai_submission_uses_bounded_task_factory()
{
    TemporaryDatabase database;
    Harness harness(database.path, true);
    auto pending = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_openai,
                           "ai-live", "réponds brièvement"});

    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = pending->wait();
    const auto task = harness.store.find("ai-live");

    expect(processed.work_may_be_pending && reply.ok,
           "enabled OpenAI command creates pending work");
    expect(task && task->kind == "provider.openai.responses",
           "live OpenAI command uses provider task kind");
    expect(task && task->limits.max_input_bytes == 16 * 1024
               && task->limits.max_output_bytes == 64 * 1024
               && task->limits.max_attempts == 2,
           "live OpenAI command reuses bounded OpenAI Task factory");
}

void test_inspect_reads_durable_task_without_submission()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);

    auto submit = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "inspect-me", "persisted"});
    static_cast<void>(harness.processor.process(harness.mailbox));
    static_cast<void>(submit->wait());

    auto inspect = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_task,
                           "inspect-me", {}});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = inspect->wait();

    expect(processed.processed == 1 && !processed.work_may_be_pending,
           "inspect does not itself request dispatch");
    expect(reply.ok && reply.body.find("id=\"inspect-me\"") != std::string::npos
               && reply.body.find("status=pending") != std::string::npos,
           "inspect returns the durable Task report");
}

void test_duplicate_preserves_original_definition()
{
    TemporaryDatabase database;
    Harness harness(database.path, false);

    auto first = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "same", "first"});
    static_cast<void>(harness.processor.process(harness.mailbox));
    static_cast<void>(first->wait());

    auto second = harness.mailbox.submit(
        LiveControlCommand{LiveControlOperation::submit_echo,
                           "same", "different"});
    const auto processed = harness.processor.process(harness.mailbox);
    const auto reply = second->wait();
    const auto task = harness.store.find("same");

    expect(reply.ok && processed.work_may_be_pending,
           "duplicate pending command is acknowledged and re-wakes dispatcher");
    expect(task && task->input == "first",
           "durable idempotency preserves original task definition");
}

} // namespace

int main()
{
    test_echo_submission_is_durable();
    test_openai_submission_requires_activated_provider();
    test_openai_submission_uses_bounded_task_factory();
    test_inspect_reads_durable_task_without_submission();
    test_duplicate_preserves_original_definition();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All live control processor tests passed\n";
    return 0;
}
