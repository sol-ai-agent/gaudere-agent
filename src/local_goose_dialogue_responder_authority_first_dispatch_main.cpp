#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;

constexpr const char* consumer_id = "system-responder-v1";
constexpr const char* thread_alias = "main";
constexpr const char* expected_grant_id =
    "cognition.local-goose-dialogue.responder-grant.v1:54f5770768e55b5bb0ddf0289c2a42555e0b8fbef6675726c4bc23b853cb2b5a";
constexpr const char* expected_lease_id =
    "authority-lease-v1-e1a58d3a65a1771c8b04b5e41d6eaeb571a7070975c73d69b2117725f00fbf63";
constexpr const char* expected_decision_id =
    "cognition.local-goose-dialogue.responder-decision.v1:5e8bfd1668346f209eaec91f5888a3a9532a5bf5677674d4c2001f499e924153";
constexpr const char* expected_intent_id =
    "cognition.local-goose-dialogue.responder-intent.v1:f82c3df882b60babea7d88a36589aad57d1f7b0b2b9ec4d208e8be8d01b71fae";
constexpr const char* expected_disposition_id =
    "cognition.local-goose-dialogue.responder-disposition.v1:72741315708884592db29c29c4e6c9d750b6e49f3b15a1bd1b61f9c1e618236b";
constexpr const char* expected_event_id =
    "cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844";
constexpr const char* expected_result_sha256 =
    "4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935";
constexpr const char* expected_trigger_task_id =
    "cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa";
constexpr const char* expected_human_speaker_id = "bertrand";
constexpr const char* responder_speaker_id = "sol";
constexpr const char* decision_message_kind = "intervention";
constexpr const char* decision_message =
    "Sol reprend le dialogue depuis le point de continuite valide.";
constexpr const char* intervention_authority_source =
    "explicit-intervention-intent-v1";
constexpr const char* model_sha256 =
    "85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87";
constexpr std::uint64_t expected_sequence = 4;
constexpr std::uint64_t expected_pre_revision = 3;
constexpr std::uint64_t expected_post_revision = 4;
constexpr std::int64_t minimum_remaining_authority_ms = 2LL * 60 * 1000;

struct Options {
    std::string command;
    std::string state;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string responder_sidecar;
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void require(const bool condition, const std::string& message)
{
    if (!condition) fail(message);
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Options parse_options(const int argc, char* argv[])
{
    if (argc >= 2 && std::string{argv[1]} == "--help") {
        std::cout << "Usage: " << argv[0]
                  << " <inspect|dispatch-intent>"
                  << " --state PATH --thread-sidecar PATH"
                  << " --completion-sidecar PATH --responder-sidecar PATH\n";
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing command");

    Options out;
    out.command = argv[1];
    if (out.command != "inspect" && out.command != "dispatch-intent") {
        throw std::invalid_argument("unsupported command: " + out.command);
    }

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    std::string("missing value for ") + name);
            }
            return argv[++i];
        };
        if (arg == "--state") out.state = need("--state");
        else if (arg == "--thread-sidecar") {
            out.thread_sidecar = need("--thread-sidecar");
        } else if (arg == "--completion-sidecar") {
            out.completion_sidecar = need("--completion-sidecar");
        } else if (arg == "--responder-sidecar") {
            out.responder_sidecar = need("--responder-sidecar");
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    for (const auto* path : {
             &out.state, &out.thread_sidecar,
             &out.completion_sidecar, &out.responder_sidecar}) {
        if (path->empty() || path->front() != '/') {
            throw std::invalid_argument("all state paths must be absolute");
        }
        const auto status = std::filesystem::symlink_status(*path);
        if (!std::filesystem::is_regular_file(status)) {
            throw std::invalid_argument(
                "state path must be an existing regular non-symlink file");
        }
    }
    return out;
}

void require_decision(
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = authority_store.find_decision(expected_decision_id);
    require(decision.has_value(), "expected authority decision is missing");
    require(decision->state
                == LocalGooseDialogueResponderDecisionState::intent_bound,
            "authority decision is not intent-bound");
    require(decision->grant_id == expected_grant_id
                && decision->lease_id == expected_lease_id,
            "authority decision grant/lease differs");
    require(decision->consumer_id == consumer_id
                && decision->completion_sequence == expected_sequence,
            "authority decision trigger differs");
    require(decision->event_id == expected_event_id
                && decision->result_sha256 == expected_result_sha256,
            "authority decision event evidence differs");
    require(decision->thread_alias == thread_alias
                && decision->thread_revision == expected_pre_revision
                && decision->task_id == expected_trigger_task_id,
            "authority decision thread evidence differs");
    require(decision->observed_speaker_kind == "human"
                && decision->observed_speaker_id == expected_human_speaker_id,
            "authority decision observed speaker differs");
    require(decision->expected_head_revision == expected_pre_revision
                && decision->expected_head_task_id == expected_trigger_task_id,
            "authority decision preferred-head evidence differs");
    require(decision->responder_speaker_kind == "system"
                && decision->responder_speaker_id == responder_speaker_id,
            "authority decision responder differs");
    require(decision->message_kind == decision_message_kind
                && decision->message == decision_message,
            "authority decision message differs");
    require(decision->intent_id
                == std::optional<std::string>{expected_intent_id}
                && decision->bound_at_ms.has_value(),
            "authority decision intent binding differs");
}

LocalGooseDialogueResponderIntent require_intent_identity(
    LocalGooseDialogueResponderStore& responder_store)
{
    const auto intent = responder_store.find_intent(expected_intent_id);
    require(intent.has_value(), "expected responder intent is missing");
    require(intent->lease_id == expected_lease_id
                && intent->thread_alias == thread_alias,
            "responder intent lease/thread differs");
    require(intent->completion_sequence == expected_sequence
                && intent->event_id == expected_event_id
                && intent->result_sha256 == expected_result_sha256,
            "responder intent trigger evidence differs");
    require(intent->triggering_task_id == expected_trigger_task_id
                && intent->expected_thread_revision == expected_pre_revision
                && intent->expected_head_task_id == expected_trigger_task_id,
            "responder intent preferred-head evidence differs");
    require(intent->speaker_kind == "system"
                && intent->speaker_id == responder_speaker_id,
            "responder intent actor differs");
    require(intent->message_kind == decision_message_kind
                && intent->message == decision_message,
            "responder intent message differs");
    return *intent;
}

void require_disposition(
    LocalGooseDialogueResponderStore& responder_store,
    const LocalGooseDialogueResponderDispositionState state)
{
    const auto disposition = responder_store.find_disposition_for_event(
        consumer_id, expected_event_id);
    require(disposition.has_value(), "sequence 4 disposition is missing");
    require(disposition->disposition_id == expected_disposition_id,
            "sequence 4 disposition identity differs");
    require(disposition->kind
                == LocalGooseDialogueResponderDispositionKind::intervention,
            "sequence 4 disposition kind differs");
    require(disposition->authority_source == intervention_authority_source,
            "sequence 4 disposition authority source differs");
    require(disposition->state == state,
            "sequence 4 disposition state differs");
    require(disposition->intent_id
                == std::optional<std::string>{expected_intent_id}
                && disposition->bound_at_ms.has_value(),
            "sequence 4 disposition intent binding differs");
}

void require_ready_state(
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    require(authority_store.schema_version()
                == local_goose_dialogue_responder_authority_sidecar_schema,
            "responder authority schema is not exactly v3");
    require(completion_store.consumer_last_sequence(consumer_id)
                == std::optional<std::uint64_t>{3},
            "responder cursor is not exactly 3 before dispatch");
    const auto event = completion_store.next_for_consumer(consumer_id);
    require(event.has_value()
                && event->sequence == expected_sequence
                && event->event_id == expected_event_id
                && event->result_sha256 == expected_result_sha256
                && event->thread_alias == thread_alias
                && event->thread_revision == expected_pre_revision
                && event->task_id == expected_trigger_task_id
                && event->speaker_kind == "human"
                && event->speaker_id == expected_human_speaker_id,
            "pending sequence 4 evidence differs before dispatch");
    const auto head = thread_store.find(thread_alias);
    require(head.has_value()
                && head->revision == expected_pre_revision
                && head->head_task_id == expected_trigger_task_id,
            "preferred head differs before dispatch");
    require(task_store.find(expected_trigger_task_id).has_value(),
            "sequence 4 predecessor Task is missing");

    const auto grant = authority_store.find_grant(expected_grant_id);
    require(grant.has_value()
                && grant->state == LocalGooseDialogueResponderGrantState::activated
                && grant->activated_lease_id
                    == std::optional<std::string>{expected_lease_id},
            "activated authority grant differs");
    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value()
                && lease->state == LocalGooseDialogueResponderLeaseState::active
                && lease->max_system_turns == 1
                && lease->turns_committed == 0,
            "authority lease is not the exact unused active lease");

    require_decision(authority_store);
    const auto intent = require_intent_identity(responder_store);
    require(intent.state == LocalGooseDialogueResponderIntentState::prepared,
            "responder intent is not prepared before dispatch");
    require(intent.submitted_task_id.empty()
                && !intent.submitted_thread_revision
                && !intent.committed_at_ms
                && !intent.completed_at_ms,
            "responder intent already has submission evidence");
    require_disposition(
        responder_store,
        LocalGooseDialogueResponderDispositionState::intervention_bound);

    const auto now = now_ms();
    require(now >= lease->issued_at_ms
                && now >= lease->next_eligible_at_ms
                && now < lease->expires_at_ms,
            "authority lease is not currently eligible");
    require(now < intent.expires_at_ms,
            "responder intent already expired");
    require(lease->expires_at_ms - now >= minimum_remaining_authority_ms,
            "less than two minutes remain on authority lease; refusing dispatch");
    require(intent.expires_at_ms - now >= minimum_remaining_authority_ms,
            "less than two minutes remain on responder intent; refusing dispatch");
}

LocalGooseDialogueResponderIntent require_dispatched_state(
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    require_decision(authority_store);
    const auto intent = require_intent_identity(responder_store);
    require(intent.state == LocalGooseDialogueResponderIntentState::completed,
            "responder intent is not completed after dispatch");
    require(!intent.submitted_task_id.empty()
                && intent.submitted_thread_revision
                    == std::optional<std::uint64_t>{expected_post_revision}
                && intent.committed_at_ms.has_value()
                && intent.completed_at_ms.has_value(),
            "completed responder intent lacks submission evidence");
    const auto task = task_store.find(intent.submitted_task_id);
    require(task.has_value(), "submitted responder Task is missing");
    const auto head = thread_store.find(thread_alias);
    require(head.has_value()
                && head->revision == expected_post_revision
                && head->head_task_id == intent.submitted_task_id,
            "preferred head did not advance exactly once");
    require(completion_store.consumer_last_sequence(consumer_id)
                == std::optional<std::uint64_t>{expected_sequence},
            "sequence 4 was not ACKed exactly");
    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value()
                && lease->turns_committed == 1
                && lease->state
                    == LocalGooseDialogueResponderLeaseState::exhausted,
            "authority lease was not exhausted after exactly one turn");
    require_disposition(
        responder_store,
        LocalGooseDialogueResponderDispositionState::completed);
    return intent;
}

void print_ready_status(
    LocalGooseDialogueResponderStore& responder_store)
{
    const auto intent = responder_store.find_intent(expected_intent_id);
    const auto lease = responder_store.find_lease(expected_lease_id);
    std::cout << "PHASE=ready\n";
    std::cout << "RESPONDER_CURSOR=3\n";
    std::cout << "NEXT_SEQUENCE=4\n";
    std::cout << "DECISION_ID=" << expected_decision_id << '\n';
    std::cout << "INTENT_ID=" << expected_intent_id << '\n';
    std::cout << "INTENT_STATE=prepared\n";
    std::cout << "INTENT_EXPIRES_AT_MS=" << intent->expires_at_ms << '\n';
    std::cout << "DISPOSITION_ID=" << expected_disposition_id << '\n';
    std::cout << "DISPOSITION_STATE=intervention_bound\n";
    std::cout << "LEASE_ID=" << expected_lease_id << '\n';
    std::cout << "LEASE_STATE=active\n";
    std::cout << "LEASE_TURNS_COMMITTED=0\n";
    std::cout << "LEASE_EXPIRES_AT_MS=" << lease->expires_at_ms << '\n';
    std::cout << "SEQUENCE4_ACK=no\n";
}

void print_dispatched_status(
    gaudere::work::TaskStore& task_store,
    const LocalGooseDialogueResponderIntent& intent)
{
    const auto task = task_store.find(intent.submitted_task_id);
    std::cout << "PHASE=dispatched\n";
    std::cout << "RESPONDER_CURSOR=4\n";
    std::cout << "DECISION_ID=" << expected_decision_id << '\n';
    std::cout << "INTENT_ID=" << expected_intent_id << '\n';
    std::cout << "INTENT_STATE=completed\n";
    std::cout << "SUBMITTED_TASK_ID=" << intent.submitted_task_id << '\n';
    std::cout << "HEAD_REVISION=4\n";
    std::cout << "DISPOSITION_ID=" << expected_disposition_id << '\n';
    std::cout << "DISPOSITION_STATE=completed\n";
    std::cout << "LEASE_ID=" << expected_lease_id << '\n';
    std::cout << "LEASE_STATE=exhausted\n";
    std::cout << "LEASE_TURNS_COMMITTED=1\n";
    std::cout << "SEQUENCE4_ACK=yes\n";
    std::cout << "TASK_TERMINAL="
              << (task && gaudere::work::is_terminal(task->status) ? "yes" : "no")
              << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        gaudere::persistence::sqlite::TaskStore task_store(options.state);
        gaudere::work::Runtime runtime(
            task_store,
            [] { return std::chrono::system_clock::now(); });
        LocalGooseDialogueThreadStore thread_store(options.thread_sidecar);
        LocalGooseDialogueCompletionFeedStore completion_store(
            options.completion_sidecar);
        LocalGooseDialogueResponderStore responder_store(
            options.responder_sidecar);
        LocalGooseDialogueResponderAuthorityStore authority_store(
            options.responder_sidecar);
        LocalGooseDialogueResponderDispatcher dispatcher(
            runtime, task_store, thread_store, completion_store,
            responder_store, model_sha256, consumer_id, now_ms);

        const auto cursor = completion_store.consumer_last_sequence(consumer_id);
        require(cursor.has_value(), "responder consumer cursor is missing");

        if (options.command == "inspect") {
            if (*cursor == 3) {
                require_ready_state(
                    task_store, completion_store, thread_store,
                    responder_store, authority_store);
                print_ready_status(responder_store);
            } else if (*cursor == expected_sequence) {
                const auto intent = require_dispatched_state(
                    task_store, completion_store, thread_store,
                    responder_store, authority_store);
                print_dispatched_status(task_store, intent);
            } else {
                fail("responder cursor is neither pre-dispatch 3 nor post-dispatch 4");
            }
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DISPATCH_INSPECT=PASS\n";
            return 0;
        }

        require_ready_state(
            task_store, completion_store, thread_store,
            responder_store, authority_store);
        const auto result = dispatcher.dispatch(expected_intent_id);
        require(result.result == LocalGooseDialogueResponderDispatchCode::accepted,
                "first production responder dispatch was not newly accepted: "
                    + result.detail);
        require(result.intent.has_value()
                    && result.intent->intent_id == expected_intent_id,
                "first production responder dispatch returned another intent");
        require(result.task.has_value(),
                "first production responder dispatch returned no Task");
        require(result.head.has_value()
                    && result.head->revision == expected_post_revision
                    && result.head->head_task_id == result.task->id,
                "first production responder dispatch returned incoherent head/Task");
        require(result.consumer_last_sequence == expected_sequence,
                "first production responder dispatch did not ACK sequence 4");

        const auto intent = require_dispatched_state(
            task_store, completion_store, thread_store,
            responder_store, authority_store);
        require(intent.submitted_task_id == result.task->id,
                "durable submitted Task differs from dispatch result");

        std::cout << "ACTION=dispatch-intent\n";
        print_dispatched_status(task_store, intent);
        std::cout << "WORK_MAY_BE_PENDING="
                  << (result.work_may_be_pending ? "yes" : "no") << '\n';
        std::cout
            << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DISPATCH=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR=" << error.what() << '\n';
        return 2;
    }
}
