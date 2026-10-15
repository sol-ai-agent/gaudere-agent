#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV2.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

struct TemporaryFile {
    explicit TemporaryFile(std::string stem)
    {
        path = std::filesystem::temp_directory_path()
            / (std::move(stem) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch().count())
               + ".db");
    }

    ~TemporaryFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }

    std::filesystem::path path;
};

std::string model_sha()
{
    return std::string(64, 'b');
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

struct Scenario {
    explicit Scenario(const std::string& stem)
        : core(stem + "-core"),
          thread(stem + "-thread"),
          feed(stem + "-feed"),
          responder(stem + "-responder"),
          task_store(core.path.string()),
          runtime(task_store, [] { return std::chrono::system_clock::now(); }),
          thread_store(thread.path.string()),
          completion_store(feed.path.string()),
          responder_store(responder.path.string())
    {
        runtime.recover();

        const auto root = succeed_v2(
            make_local_goose_dialogue_v2_root_task(
                stem + "-root", "Bonjour.", model_sha()),
            "Bonjour.");
        const auto human = succeed_v2(
            make_local_goose_dialogue_v2_successor_task(
                stem + "-human", "Continue.", model_sha(), root),
            "Je continue.");
        task_store.save(root);
        task_store.save(human);

        const LocalGooseDialogueThreadHead head{
            "main", 0, root.id, human.id};
        assert(thread_store.seed(head).result
            == LocalGooseDialogueThreadStoreResult::accepted);

        const auto stored_human = task_store.find(human.id);
        assert(stored_human);
        const auto event = make_local_goose_dialogue_completion_event(
            "main", 0, *stored_human, 1000);
        const auto append = completion_store.append_for_revision(event);
        assert(append.result
            == LocalGooseDialogueCompletionFeedResult::accepted);
        assert(append.event && append.event->sequence == 1);
        human_event = *append.event;
    }

    TemporaryFile core;
    TemporaryFile thread;
    TemporaryFile feed;
    TemporaryFile responder;
    gaudere::persistence::sqlite::TaskStore task_store;
    gaudere::work::Runtime runtime;
    LocalGooseDialogueThreadStore thread_store;
    LocalGooseDialogueCompletionFeedStore completion_store;
    LocalGooseDialogueResponderStore responder_store;
    LocalGooseDialogueCompletionEvent human_event;
};

void assert_bound(
    LocalGooseDialogueResponderStore& store,
    const std::string& consumer_id,
    const LocalGooseDialogueCompletionEvent& event,
    const std::string& intent_id)
{
    const auto disposition = store.find_disposition_for_event(
        consumer_id, event.event_id);
    assert(disposition);
    assert(disposition->kind
        == LocalGooseDialogueResponderDispositionKind::intervention);
    assert(disposition->state
        == LocalGooseDialogueResponderDispositionState::intervention_bound);
    assert(disposition->intent_id
        == std::optional<std::string>{intent_id});
}

void assert_terminal_pair(
    LocalGooseDialogueResponderStore& store,
    const std::string& consumer_id,
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueResponderIntentState intent_state,
    const std::string& reason,
    const std::optional<std::int64_t>& expected_terminal_at = std::nullopt)
{
    const auto disposition = store.find_disposition_for_event(
        consumer_id, event.event_id);
    assert(disposition);
    assert(disposition->state
        == LocalGooseDialogueResponderDispositionState::manual_review);
    assert(disposition->terminal_reason == reason);
    assert(disposition->terminal_at_ms);
    if (expected_terminal_at) {
        assert(disposition->terminal_at_ms == expected_terminal_at);
    }
    assert(disposition->intent_id);

    const auto intent = store.find_intent(*disposition->intent_id);
    assert(intent);
    assert(intent->state == intent_state);
    assert(intent->terminal_reason == reason);
}

void prove_normal_terminal_pairing()
{
    Scenario scenario{"terminal-pair"};
    constexpr const char* consumer_id = "terminal-pair-consumer";
    std::int64_t now = 2000;

    LocalGooseDialogueResponderDispatcher dispatcher(
        scenario.runtime,
        scenario.task_store,
        scenario.thread_store,
        scenario.completion_store,
        scenario.responder_store,
        model_sha(),
        consumer_id,
        [&now] { return now; });

    const auto lease = dispatcher.create_lease(
        "terminal-pair-lease", "main", "sol", "feedback",
        "Terminal pairing proof", 1, 1000, 0);
    assert(lease.result == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(lease.lease);

    const auto prepared = dispatcher.prepare(
        lease.lease->lease_id,
        scenario.human_event.sequence,
        "feedback",
        "Cette intervention expirera avant dispatch.",
        1000);
    assert(prepared.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared.intent);
    assert_bound(
        scenario.responder_store, consumer_id,
        scenario.human_event, prepared.intent->intent_id);

    now = 3000;
    const auto terminal = dispatcher.dispatch(prepared.intent->intent_id);
    assert(terminal.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(terminal.intent);
    assert(terminal.intent->state
        == LocalGooseDialogueResponderIntentState::expired);
    assert(terminal.intent->terminal_reason == "lease_or_intent_expired");
    assert_terminal_pair(
        scenario.responder_store, consumer_id,
        scenario.human_event,
        LocalGooseDialogueResponderIntentState::expired,
        "lease_or_intent_expired",
        std::optional<std::int64_t>{now});
    assert(scenario.completion_store.consumer_last_sequence(consumer_id)
        == std::optional<std::uint64_t>{0});
    const auto final_lease = scenario.responder_store.find_lease(
        lease.lease->lease_id);
    assert(final_lease && final_lease->turns_committed == 0);

    const auto disposition = scenario.responder_store.find_disposition_for_event(
        consumer_id, scenario.human_event.event_id);
    assert(disposition && disposition->terminal_at_ms);
    const auto terminal_at = disposition->terminal_at_ms;

    now = 3100;
    LocalGooseDialogueResponderDispatcher restarted(
        scenario.runtime,
        scenario.task_store,
        scenario.thread_store,
        scenario.completion_store,
        scenario.responder_store,
        model_sha(),
        consumer_id,
        [&now] { return now; });
    const auto replay = restarted.dispatch(prepared.intent->intent_id);
    assert(replay.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert_terminal_pair(
        scenario.responder_store, consumer_id,
        scenario.human_event,
        LocalGooseDialogueResponderIntentState::expired,
        "lease_or_intent_expired",
        terminal_at);
    assert(scenario.completion_store.consumer_last_sequence(consumer_id)
        == std::optional<std::uint64_t>{0});
    assert(scenario.responder_store.find_lease(
        lease.lease->lease_id)->turns_committed == 0);
}

void prove_crash_window_reconciliation()
{
    Scenario scenario{"terminal-crash"};
    constexpr const char* consumer_id = "terminal-crash-consumer";
    std::int64_t now = 4000;

    LocalGooseDialogueResponderDispatcher dispatcher(
        scenario.runtime,
        scenario.task_store,
        scenario.thread_store,
        scenario.completion_store,
        scenario.responder_store,
        model_sha(),
        consumer_id,
        [&now] { return now; });

    const auto lease = dispatcher.create_lease(
        "terminal-crash-lease", "main", "sol", "feedback",
        "Crash reconciliation proof", 1, 10000, 0);
    assert(lease.result == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(lease.lease);

    const auto prepared = dispatcher.prepare(
        lease.lease->lease_id,
        scenario.human_event.sequence,
        "feedback",
        "Simulation du crash entre les deux écritures terminales.",
        5000);
    assert(prepared.result
        == LocalGooseDialogueResponderDispatchCode::accepted);
    assert(prepared.intent);
    assert_bound(
        scenario.responder_store, consumer_id,
        scenario.human_event, prepared.intent->intent_id);

    const auto terminal_intent = scenario.responder_store.terminalize_intent(
        prepared.intent->intent_id,
        LocalGooseDialogueResponderIntentState::conflict,
        "simulated_crash_after_intent_terminal");
    assert(terminal_intent.result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(terminal_intent.intent);
    assert_bound(
        scenario.responder_store, consumer_id,
        scenario.human_event, prepared.intent->intent_id);

    now = 4100;
    LocalGooseDialogueResponderDispatcher restarted(
        scenario.runtime,
        scenario.task_store,
        scenario.thread_store,
        scenario.completion_store,
        scenario.responder_store,
        model_sha(),
        consumer_id,
        [&now] { return now; });
    const auto recovered = restarted.dispatch(prepared.intent->intent_id);
    assert(recovered.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(recovered.detail == "terminal responder disposition reconciled");
    assert_terminal_pair(
        scenario.responder_store, consumer_id,
        scenario.human_event,
        LocalGooseDialogueResponderIntentState::conflict,
        "simulated_crash_after_intent_terminal",
        std::optional<std::int64_t>{now});
    assert(scenario.completion_store.consumer_last_sequence(consumer_id)
        == std::optional<std::uint64_t>{0});
    assert(scenario.responder_store.find_lease(
        lease.lease->lease_id)->turns_committed == 0);

    const auto disposition = scenario.responder_store.find_disposition_for_event(
        consumer_id, scenario.human_event.event_id);
    assert(disposition && disposition->terminal_at_ms);
    const auto terminal_at = disposition->terminal_at_ms;

    now = 4200;
    LocalGooseDialogueResponderDispatcher restarted_again(
        scenario.runtime,
        scenario.task_store,
        scenario.thread_store,
        scenario.completion_store,
        scenario.responder_store,
        model_sha(),
        consumer_id,
        [&now] { return now; });
    const auto replay = restarted_again.dispatch(prepared.intent->intent_id);
    assert(replay.result
        == LocalGooseDialogueResponderDispatchCode::conflict);
    assert(replay.detail
        == "dialogue responder intent and disposition are terminal");
    assert_terminal_pair(
        scenario.responder_store, consumer_id,
        scenario.human_event,
        LocalGooseDialogueResponderIntentState::conflict,
        "simulated_crash_after_intent_terminal",
        terminal_at);
    assert(scenario.completion_store.consumer_last_sequence(consumer_id)
        == std::optional<std::uint64_t>{0});
    assert(scenario.responder_store.find_lease(
        lease.lease->lease_id)->turns_committed == 0);
}

} // namespace

int main()
{
    prove_normal_terminal_pairing();
    prove_crash_window_reconciliation();
    std::cout
        << "local_goose_dialogue_responder_terminal_reconciliation_test: PASS\n";
    return 0;
}
