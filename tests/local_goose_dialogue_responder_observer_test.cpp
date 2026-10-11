#include "LocalGooseDialogueResponderObserver.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using Json = nlohmann::json;
using Classification = LocalGooseDialogueResponderObserverClassification;
using Action = LocalGooseDialogueResponderObserverAction;
using FeedResult = LocalGooseDialogueCompletionFeedResult;
using ThreadResult = LocalGooseDialogueThreadStoreResult;
using StoreResult = LocalGooseDialogueResponderStoreResult;
using DispositionKind = LocalGooseDialogueResponderDispositionKind;
using DispositionState = LocalGooseDialogueResponderDispositionState;

std::string v3_id(const char digit)
{
    return "cognition.local-goose-dialogue.v3:" + std::string(64, digit);
}

struct TemporaryState {
    explicit TemporaryState(std::string label)
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto base = std::filesystem::temp_directory_path()
            / ("gaudere-responder-observer-" + std::move(label) + "-" + suffix);
        thread = base.string() + "-thread.db";
        feed = base.string() + "-feed.db";
        responder = base.string() + "-responder.db";
    }

    ~TemporaryState()
    {
        std::error_code ignored;
        for (const auto& path : {thread, feed, responder}) {
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path + "-journal", ignored);
        }
    }

    std::string thread;
    std::string feed;
    std::string responder;
};

LocalGooseDialogueThreadHead head(
    const std::uint64_t revision,
    const std::string& root_task_id,
    const std::string& task_id)
{
    LocalGooseDialogueThreadHead out;
    out.alias = "main";
    out.revision = revision;
    out.root_task_id = root_task_id;
    out.head_task_id = task_id;
    assert(valid_local_goose_dialogue_thread_head(out));
    return out;
}

void seed_and_advance(
    LocalGooseDialogueThreadStore& store,
    const std::string& root_task_id,
    const std::initializer_list<std::string>& tasks)
{
    assert(tasks.size() > 0);
    auto it = tasks.begin();
    auto current = head(0, root_task_id, *it);
    assert(store.seed(current).result == ThreadResult::accepted);
    ++it;
    for (; it != tasks.end(); ++it) {
        auto replacement = head(current.revision + 1, root_task_id, *it);
        assert(store.replace(current, replacement).result == ThreadResult::accepted);
        current = std::move(replacement);
    }
}

std::string event_identity(
    const std::string& thread_alias,
    const std::uint64_t revision,
    const std::string& task_id,
    const std::string& result_sha256)
{
    const Json identity{
        {"result_sha256", result_sha256},
        {"schema", "gaudere.cognition.local-goose-dialogue.completion.identity.v1"},
        {"task_id", task_id},
        {"thread_alias", thread_alias},
        {"thread_revision", revision}
    };
    return std::string{local_goose_dialogue_completion_event_prefix}
        + sha256_hex(identity.dump());
}

LocalGooseDialogueCompletionEvent event(
    const std::uint64_t revision,
    const std::string& task_id,
    const std::string& root_task_id,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const char result_digit,
    const std::int64_t observed_at_ms)
{
    LocalGooseDialogueCompletionEvent out;
    out.thread_alias = "main";
    out.thread_revision = revision;
    out.task_id = task_id;
    out.root_task_id = root_task_id;
    out.turn_index = revision;
    out.request_id = "request-" + std::to_string(revision);
    out.speaker_kind = speaker_kind;
    out.speaker_id = speaker_id;
    out.message_kind = speaker_kind == "human" ? "dialogue" : "feedback";
    out.result_sha256 = std::string(64, result_digit);
    out.response = "response-" + std::to_string(revision);
    out.observed_completed_at_ms = observed_at_ms;
    out.event_id = event_identity(
        out.thread_alias, out.thread_revision, out.task_id, out.result_sha256);
    assert(valid_local_goose_dialogue_completion_event(out));
    return out;
}

LocalGooseDialogueCompletionEvent append(
    LocalGooseDialogueCompletionFeedStore& feed,
    const LocalGooseDialogueCompletionEvent& input)
{
    const auto write = feed.append_for_revision(input);
    assert(write.result == FeedResult::accepted);
    assert(write.event);
    return *write.event;
}

LocalGooseDialogueResponderObserverConfig armed_config()
{
    LocalGooseDialogueResponderObserverConfig config;
    config.observation_armed = true;
    return config;
}

LocalGooseDialogueResponderExplicitDecline decline_for(
    const LocalGooseDialogueCompletionEvent& event,
    const LocalGooseDialogueThreadHead& preferred_head)
{
    LocalGooseDialogueResponderExplicitDecline authorization;
    authorization.completion_sequence = event.sequence;
    authorization.event_id = event.event_id;
    authorization.result_sha256 = event.result_sha256;
    authorization.thread_revision = event.thread_revision;
    authorization.task_id = event.task_id;
    authorization.expected_head_revision = preferred_head.revision;
    authorization.expected_head_task_id = preferred_head.head_task_id;
    authorization.authority_source = "operator-v1";
    return authorization;
}

} // namespace

int main()
{
    // Post-9K5 shape: system completion followed by current human head.
    // Disarmed code is read-only; armed observer retires exactly the system
    // event and then stops on the human event.
    {
        TemporaryState state{"backlog"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);

        const auto root = v3_id('1');
        const auto system_task = v3_id('2');
        const auto human_task = v3_id('3');
        seed_and_advance(threads, root, {system_task, human_task});
        const auto system_event = append(
            feed, event(0, system_task, root, "system", "sol", 'a', 1000));
        const auto human_event = append(
            feed, event(1, human_task, root, "human", "bertrand", 'b', 1100));
        assert(system_event.sequence == 1);
        assert(human_event.sequence == 2);

        std::int64_t now = 2000;
        LocalGooseDialogueResponderObserver disarmed(
            feed, threads, responders, {}, [&] { return now++; });
        const auto disarmed_status = disarmed.inspect();
        assert(disarmed_status.classification == Classification::structural_system);
        assert(!disarmed_status.consumer_last_sequence);
        assert(disarmed.step().action == Action::disarmed);
        assert(!feed.consumer_last_sequence("system-responder-v1"));
        assert(responders.dispositions_in_state(
            DispositionState::prepared, 64).empty());

        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        const auto first = observer.step();
        assert(first.action == Action::acknowledged);
        assert(feed.consumer_last_sequence("system-responder-v1") == 1);
        const auto system_disposition = responders.find_disposition_for_event(
            "system-responder-v1", system_event.event_id);
        assert(system_disposition);
        assert(system_disposition->kind == DispositionKind::structural_skip_system);
        assert(system_disposition->state == DispositionState::acknowledged);

        const auto held = observer.step();
        assert(held.action == Action::held);
        assert(held.status.classification == Classification::current_human);
        assert(held.status.next_event);
        assert(held.status.next_event->event_id == human_event.event_id);
        assert(feed.consumer_last_sequence("system-responder-v1") == 1);
        assert(!responders.find_disposition_for_event(
            "system-responder-v1", human_event.event_id));

        auto wrong_decline = decline_for(human_event, *threads.find("main"));
        wrong_decline.result_sha256 = std::string(64, 'f');
        assert(observer.decline(wrong_decline).action == Action::held);
        assert(feed.consumer_last_sequence("system-responder-v1") == 1);

        const auto accepted_decline = observer.decline(
            decline_for(human_event, *threads.find("main")));
        assert(accepted_decline.action == Action::acknowledged);
        assert(feed.consumer_last_sequence("system-responder-v1") == 2);
        const auto decline_disposition = responders.find_disposition_for_event(
            "system-responder-v1", human_event.event_id);
        assert(decline_disposition);
        assert(decline_disposition->kind == DispositionKind::explicit_decline);
        assert(decline_disposition->state == DispositionState::acknowledged);
        assert(observer.inspect().classification == Classification::no_event);
    }

    // Crash after the feed ACK but before responder-sidecar acknowledgement:
    // the next run reconciles from the exact cursor and never ACKs a new event.
    {
        TemporaryState state{"after-ack-crash"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('4');
        const auto task = v3_id('5');
        seed_and_advance(threads, root, {task});
        const auto completed = append(
            feed, event(0, task, root, "system", "sol", 'c', 3000));
        auto disposition = make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", completed.sequence,
            completed.event_id, completed.result_sha256, completed.thread_revision,
            completed.task_id, completed.speaker_kind, completed.speaker_id,
            DispositionKind::structural_skip_system, "observer-policy-v1", 3100);
        assert(responders.prepare_disposition(disposition).result
            == StoreResult::accepted);
        assert(feed.acknowledge("system-responder-v1", completed.sequence).result
            == FeedResult::accepted);
        assert(responders.find_disposition(disposition.disposition_id)->state
            == DispositionState::prepared);

        std::int64_t now = 3200;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        const auto recovered = observer.step();
        assert(recovered.action == Action::reconciled);
        assert(feed.consumer_last_sequence("system-responder-v1")
            == completed.sequence);
        assert(responders.find_disposition(disposition.disposition_id)->state
            == DispositionState::acknowledged);
    }

    // Crash after durable disposition preparation but before feed ACK: retry
    // validates exact next-event evidence, ACKs only that sequence, and closes
    // the prepared decision idempotently.
    {
        TemporaryState state{"before-ack-crash"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('6');
        const auto task = v3_id('7');
        seed_and_advance(threads, root, {task});
        const auto completed = append(
            feed, event(0, task, root, "system", "sol", 'd', 4000));
        auto disposition = make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", completed.sequence,
            completed.event_id, completed.result_sha256, completed.thread_revision,
            completed.task_id, completed.speaker_kind, completed.speaker_id,
            DispositionKind::structural_skip_system, "observer-policy-v1", 4100);
        assert(responders.prepare_disposition(disposition).result
            == StoreResult::accepted);
        assert(!feed.consumer_last_sequence("system-responder-v1"));

        std::int64_t now = 4200;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        const auto recovered = observer.step();
        assert(recovered.action == Action::acknowledged);
        assert(feed.consumer_last_sequence("system-responder-v1")
            == completed.sequence);
        assert(responders.find_disposition(disposition.disposition_id)->state
            == DispositionState::acknowledged);
    }

    // A human event that is no longer the preferred head is structurally stale.
    // The observer may retire it, but then must stop on the new current human.
    {
        TemporaryState state{"stale"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('8');
        const auto system_task = v3_id('9');
        const auto stale_human_task = v3_id('a');
        const auto current_human_task = v3_id('b');
        seed_and_advance(
            threads, root, {system_task, stale_human_task, current_human_task});
        append(feed, event(0, system_task, root, "system", "sol", '1', 5000));
        const auto stale_human = append(
            feed, event(1, stale_human_task, root, "human", "bertrand", '2', 5100));
        const auto current_human = append(
            feed, event(2, current_human_task, root, "human", "bertrand", '3', 5200));

        std::int64_t now = 5300;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        assert(observer.step().action == Action::acknowledged);
        const auto stale_step = observer.step();
        assert(stale_step.action == Action::acknowledged);
        const auto stale_disposition = responders.find_disposition_for_event(
            "system-responder-v1", stale_human.event_id);
        assert(stale_disposition);
        assert(stale_disposition->kind == DispositionKind::structural_skip_stale);
        assert(feed.consumer_last_sequence("system-responder-v1") == 2);
        const auto current = observer.step();
        assert(current.action == Action::held);
        assert(current.status.classification == Classification::current_human);
        assert(current.status.next_event->event_id == current_human.event_id);
        assert(feed.consumer_last_sequence("system-responder-v1") == 2);
    }

    // Same revision with a different Task ID is impossible preferred-head
    // evidence, not a stale event. Fail closed with no disposition and no ACK.
    {
        TemporaryState state{"ambiguous"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('c');
        const auto preferred = v3_id('d');
        const auto conflicting = v3_id('e');
        seed_and_advance(threads, root, {preferred});
        const auto completed = append(
            feed, event(0, conflicting, root, "human", "bertrand", '4', 6000));

        std::int64_t now = 6100;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        const auto status = observer.inspect();
        assert(status.classification == Classification::ambiguous);
        assert(observer.step().action == Action::held);
        assert(!feed.consumer_last_sequence("system-responder-v1"));
        assert(!responders.find_disposition_for_event(
            "system-responder-v1", completed.event_id));
    }

    // An intervention disposition is never consumed by observer authority.
    {
        TemporaryState state{"intervention-owned"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('f');
        const auto human_task = v3_id('0');
        seed_and_advance(threads, root, {human_task});
        const auto completed = append(
            feed, event(0, human_task, root, "human", "bertrand", '5', 7000));
        auto disposition = make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", completed.sequence,
            completed.event_id, completed.result_sha256, completed.thread_revision,
            completed.task_id, completed.speaker_kind, completed.speaker_id,
            DispositionKind::intervention, "operator-v1", 7100);
        assert(responders.prepare_disposition(disposition).result
            == StoreResult::accepted);

        std::int64_t now = 7200;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        assert(observer.inspect().classification == Classification::intervention_owned);
        assert(observer.step().action == Action::held);
        assert(!feed.consumer_last_sequence("system-responder-v1"));
        assert(responders.find_disposition(disposition.disposition_id)->state
            == DispositionState::prepared);
    }

    // A prepared disposition that cannot explain the next cursor transition is
    // terminalized for manual review; the observer does not skip the gap.
    {
        TemporaryState state{"gap"};
        LocalGooseDialogueThreadStore threads(state.thread);
        LocalGooseDialogueCompletionFeedStore feed(state.feed);
        LocalGooseDialogueResponderStore responders(state.responder);
        const auto root = v3_id('1');
        const auto task0 = v3_id('2');
        const auto task1 = v3_id('3');
        seed_and_advance(threads, root, {task0, task1});
        append(feed, event(0, task0, root, "system", "sol", '6', 8000));
        const auto second = append(
            feed, event(1, task1, root, "system", "sol", '7', 8100));
        auto unexplained = make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", second.sequence,
            second.event_id, second.result_sha256, second.thread_revision,
            second.task_id, second.speaker_kind, second.speaker_id,
            DispositionKind::structural_skip_system, "observer-policy-v1", 8200);
        assert(responders.prepare_disposition(unexplained).result
            == StoreResult::accepted);

        std::int64_t now = 8300;
        LocalGooseDialogueResponderObserver observer(
            feed, threads, responders, armed_config(), [&] { return now++; });
        const auto result = observer.step();
        assert(result.action == Action::manual_review);
        assert(!feed.consumer_last_sequence("system-responder-v1"));
        const auto reviewed = responders.find_disposition(unexplained.disposition_id);
        assert(reviewed);
        assert(reviewed->state == DispositionState::manual_review);
    }

    std::cout << "local_goose_dialogue_responder_observer_test: PASS\n";
    return 0;
}
