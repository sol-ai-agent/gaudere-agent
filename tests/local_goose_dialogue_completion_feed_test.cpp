#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"

#include <gaudere/work/Task.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

struct TemporarySidecar {
    explicit TemporarySidecar(std::string stem)
    {
        path = std::filesystem::temp_directory_path()
            / (std::move(stem) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch().count())
               + ".db");
    }

    ~TemporarySidecar()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
    }

    std::filesystem::path path;
};

std::string model_sha()
{
    return std::string(64, 'a');
}

Task succeed_v2(Task task, const std::string& response)
{
    const auto inspected = inspect_local_goose_dialogue_v2_task(task);
    assert(inspected.eligible);
    task.status = TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = TaskResult{
        local_goose_dialogue_v2_response_content_type,
        make_local_goose_dialogue_v2_response(inspected, response),
        {},
        {}};
    assert(canonical_local_goose_dialogue_v2_success(task));
    return task;
}

Task succeed_v3(Task task, const std::string& response)
{
    const auto inspected = inspect_local_goose_dialogue_v3_task(task);
    assert(inspected.eligible);
    task.status = TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = TaskResult{
        local_goose_dialogue_v3_response_content_type,
        make_local_goose_dialogue_v3_response(inspected, response),
        {},
        {}};
    assert(canonical_local_goose_dialogue_v3_success(task));
    return task;
}

} // namespace

int main()
{
    TemporarySidecar thread_sidecar{"gaudere-dialogue-thread-history-test"};
    TemporarySidecar feed_sidecar{"gaudere-dialogue-completion-feed-test"};

    LocalGooseDialogueThreadStore thread_store(thread_sidecar.path.string());
    LocalGooseDialogueCompletionFeedStore feed_store(feed_sidecar.path.string());

    struct stat feed_status {};
    assert(::stat(feed_sidecar.path.c_str(), &feed_status) == 0);
    assert(S_ISREG(feed_status.st_mode));
    assert((feed_status.st_mode & 0777) == 0600);

    const auto model = model_sha();
    const auto v2_root = succeed_v2(
        make_local_goose_dialogue_v2_root_task(
            "feed-v2-root", "Bonjour.", model),
        "Bonjour Bertrand.");
    const auto v2_next = succeed_v2(
        make_local_goose_dialogue_v2_successor_task(
            "feed-v2-next", "Suite.", model, v2_root),
        "Je conserve le contexte.");

    std::unordered_map<std::string, Task> tasks;
    tasks.emplace(v2_root.id, v2_root);
    tasks.emplace(v2_next.id, v2_next);

    LocalGooseDialogueThreadHead initial{
        "main", 0, v2_root.id, v2_next.id};
    assert(thread_store.seed(initial).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    std::int64_t now = 1000;
    LocalGooseDialogueCompletionFeedService service(
        thread_store,
        feed_store,
        [&tasks](const std::string& id) -> std::optional<Task> {
            const auto found = tasks.find(id);
            return found == tasks.end()
                ? std::nullopt
                : std::optional<Task>{found->second};
        },
        [&now] { return now++; });

    const auto first = service.reconcile("main");
    assert(!first.blocked && !first.pending && first.materialized == 1);
    assert(feed_store.materialization_next_revision("main")
        == std::optional<std::uint64_t>{1});

    const auto first_event =
        feed_store.next_for_consumer("gaudere-runtime");
    assert(first_event);
    assert(first_event->sequence == 1);
    assert(first_event->thread_alias == "main");
    assert(first_event->thread_revision == 0);
    assert(first_event->task_id == v2_next.id);
    assert(first_event->root_task_id == v2_root.id);
    assert(first_event->turn_index == 1);
    assert(first_event->speaker_kind == "human");
    assert(first_event->speaker_id == "legacy-v2-human");
    assert(first_event->message_kind == "dialogue");
    assert(first_event->response == "Je conserve le contexte.");

    const auto first_ack =
        feed_store.acknowledge("gaudere-runtime", first_event->sequence);
    assert(first_ack.result == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(!feed_store.next_for_consumer("gaudere-runtime"));
    const auto duplicate_ack =
        feed_store.acknowledge("gaudere-runtime", first_event->sequence);
    assert(duplicate_ack.result
        == LocalGooseDialogueCompletionFeedResult::duplicate);

    const auto bridge = succeed_v3(
        make_local_goose_dialogue_v3_bridge_from_v2_task(
            "feed-v3-bridge",
            "system",
            "sol",
            "observation",
            "Le système rejoint ce fil.",
            model,
            v2_next),
        "Observation système reçue.");
    tasks.emplace(bridge.id, bridge);

    auto revision1 = initial;
    revision1.revision = 1;
    revision1.head_task_id = bridge.id;
    assert(thread_store.replace(initial, revision1).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto second = service.reconcile("main");
    assert(!second.blocked && !second.pending && second.materialized == 1);
    assert(feed_store.materialization_next_revision("main")
        == std::optional<std::uint64_t>{2});

    const auto second_event =
        feed_store.next_for_consumer("gaudere-runtime");
    assert(second_event);
    assert(second_event->sequence == 2);
    assert(second_event->thread_revision == 1);
    assert(second_event->task_id == bridge.id);
    assert(second_event->root_task_id == v2_root.id);
    assert(second_event->turn_index == 2);
    assert(second_event->speaker_kind == "system");
    assert(second_event->speaker_id == "sol");
    assert(second_event->message_kind == "observation");
    assert(second_event->response == "Observation système reçue.");

    const auto fresh_consumer_event =
        feed_store.next_for_consumer("worker-dev");
    assert(fresh_consumer_event);
    assert(fresh_consumer_event->sequence == 1);
    const auto skip =
        feed_store.acknowledge("worker-dev", second_event->sequence);
    assert(skip.result == LocalGooseDialogueCompletionFeedResult::conflict);

    const auto second_ack =
        feed_store.acknowledge("gaudere-runtime", second_event->sequence);
    assert(second_ack.result
        == LocalGooseDialogueCompletionFeedResult::accepted);

    const auto replay = service.reconcile("main");
    assert(!replay.blocked && !replay.pending && replay.materialized == 0);

    auto feedback = make_local_goose_dialogue_v3_successor_task(
        "feed-v3-feedback",
        "system",
        "gaudere-runtime",
        "feedback",
        "Retour système.",
        model,
        bridge);
    tasks.emplace(feedback.id, feedback);

    auto revision2 = revision1;
    revision2.revision = 2;
    revision2.head_task_id = feedback.id;
    assert(thread_store.replace(revision1, revision2).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto pending = service.reconcile("main");
    assert(!pending.blocked && pending.pending && pending.materialized == 0);
    assert(feed_store.materialization_next_revision("main")
        == std::optional<std::uint64_t>{2});

    feedback = succeed_v3(feedback, "Merci pour ce retour.");
    tasks[feedback.id] = feedback;
    const auto third = service.reconcile("main");
    assert(!third.blocked && !third.pending && third.materialized == 1);
    const auto third_event =
        feed_store.next_for_consumer("gaudere-runtime");
    assert(third_event);
    assert(third_event->sequence == 3);
    assert(third_event->speaker_id == "gaudere-runtime");
    assert(third_event->message_kind == "feedback");
    assert(third_event->response == "Merci pour ce retour.");

    auto intervention = make_local_goose_dialogue_v3_successor_task(
        "feed-v3-terminal-failure",
        "system",
        "sol",
        "intervention",
        "Intervention.",
        model,
        feedback);
    intervention.status = TaskStatus::failed;
    intervention.attempts_started = 1;
    intervention.result = TaskResult{
        {},
        {},
        "test_failure",
        "terminal fixture"};
    tasks.emplace(intervention.id, intervention);

    auto revision3 = revision2;
    revision3.revision = 3;
    revision3.head_task_id = intervention.id;
    assert(thread_store.replace(revision2, revision3).result
        == LocalGooseDialogueThreadStoreResult::accepted);

    const auto blocked = service.reconcile("main");
    assert(blocked.blocked && !blocked.pending && blocked.materialized == 0);
    assert(feed_store.materialization_next_revision("main")
        == std::optional<std::uint64_t>{3});

    const auto history = thread_store.history_from("main", 0, 16);
    assert(history.size() == 4);
    assert(history[0].revision == 0 && history[0].head_task_id == v2_next.id);
    assert(history[1].revision == 1 && history[1].head_task_id == bridge.id);
    assert(history[2].revision == 2 && history[2].head_task_id == feedback.id);
    assert(history[3].revision == 3
        && history[3].head_task_id == intervention.id);

    const auto duplicate_event =
        make_local_goose_dialogue_completion_event(
            "duplicate-baseline", 5, v2_next, 5000);
    const auto duplicate_first =
        feed_store.append_for_revision(duplicate_event);
    assert(duplicate_first.result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    const auto duplicate_retry =
        make_local_goose_dialogue_completion_event(
            "duplicate-baseline", 5, v2_next, 9000);
    const auto duplicate_second =
        feed_store.append_for_revision(duplicate_retry);
    assert(duplicate_second.result
        == LocalGooseDialogueCompletionFeedResult::duplicate);
    assert(duplicate_second.event);
    assert(duplicate_second.event->observed_completed_at_ms == 5000);

    std::cout << "local_goose_dialogue_completion_feed_test: PASS\n";
    return 0;
}
