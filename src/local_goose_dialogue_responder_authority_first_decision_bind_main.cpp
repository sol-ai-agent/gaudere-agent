#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityService.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

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
constexpr const char* authority_source = "local-typed-operator-v1";
constexpr const char* intervention_authority_source =
    "explicit-intervention-intent-v1";
constexpr const char* expected_grant_id =
    "cognition.local-goose-dialogue.responder-grant.v1:54f5770768e55b5bb0ddf0289c2a42555e0b8fbef6675726c4bc23b853cb2b5a";
constexpr const char* expected_lease_id =
    "authority-lease-v1-e1a58d3a65a1771c8b04b5e41d6eaeb571a7070975c73d69b2117725f00fbf63";
constexpr const char* expected_decision_id =
    "cognition.local-goose-dialogue.responder-decision.v1:5e8bfd1668346f209eaec91f5888a3a9532a5bf5677674d4c2001f499e924153";
constexpr const char* decision_request_id =
    "first-production-responder-authority-decision-seq4-v1";
constexpr const char* responder_speaker_id = "sol";
constexpr const char* decision_message_kind = "intervention";
constexpr const char* decision_message =
    "Sol reprend le dialogue depuis le point de continuite valide.";
constexpr std::int64_t decision_intent_ttl_ms = 30LL * 60 * 1000;
constexpr std::int64_t minimum_remaining_lease_ms = 5LL * 60 * 1000;
constexpr std::uint64_t expected_sequence = 4;
constexpr std::uint64_t expected_thread_revision = 3;
constexpr const char* expected_event_id =
    "cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844";
constexpr const char* expected_result_sha256 =
    "4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935";
constexpr const char* expected_task_id =
    "cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa";
constexpr const char* expected_human_speaker_id = "bertrand";
constexpr const char* model_sha256 =
    "85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87";

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
        std::cout
            << "Usage: " << argv[0]
            << " <inspect|bind-decision>"
            << " --state PATH --thread-sidecar PATH"
            << " --completion-sidecar PATH --responder-sidecar PATH\n";
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing command");

    Options out;
    out.command = argv[1];
    if (out.command != "inspect" && out.command != "bind-decision") {
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

void require_decision_identity(
    const LocalGooseDialogueResponderDecision& decision)
{
    require(decision.decision_id == expected_decision_id,
            "authority decision identity differs");
    require(decision.request_id == decision_request_id,
            "authority decision request identity differs");
    require(decision.authority_source == authority_source,
            "authority decision source differs");
    require(decision.grant_id == expected_grant_id,
            "authority decision grant differs");
    require(decision.lease_id == expected_lease_id,
            "authority decision lease differs");
    require(decision.consumer_id == consumer_id,
            "authority decision consumer differs");
    require(decision.completion_sequence == expected_sequence,
            "authority decision sequence differs");
    require(decision.event_id == expected_event_id,
            "authority decision event differs");
    require(decision.result_sha256 == expected_result_sha256,
            "authority decision result hash differs");
    require(decision.thread_alias == thread_alias
                && decision.thread_revision == expected_thread_revision,
            "authority decision thread evidence differs");
    require(decision.task_id == expected_task_id,
            "authority decision Task differs");
    require(decision.observed_speaker_kind == "human"
                && decision.observed_speaker_id == expected_human_speaker_id,
            "authority decision observed speaker differs");
    require(decision.expected_head_revision == expected_thread_revision
                && decision.expected_head_task_id == expected_task_id,
            "authority decision preferred-head evidence differs");
    require(decision.responder_speaker_kind == "system"
                && decision.responder_speaker_id == responder_speaker_id,
            "authority decision responder differs");
    require(decision.message_kind == decision_message_kind,
            "authority decision message kind differs");
    require(decision.message == decision_message,
            "authority decision message bytes differ");
    require(decision.intent_ttl_ms == decision_intent_ttl_ms,
            "authority decision intent TTL differs");
}

LocalGooseDialogueResponderLease require_canonical_checkpoint(
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
            "responder cursor is not exactly 3");

    const auto event = completion_store.next_for_consumer(consumer_id);
    require(event.has_value() && event->sequence == expected_sequence,
            "pending sequence 4 event is missing");
    require(event->event_id == expected_event_id
                && event->result_sha256 == expected_result_sha256,
            "pending sequence 4 event evidence differs");
    require(event->thread_alias == thread_alias
                && event->thread_revision == expected_thread_revision
                && event->task_id == expected_task_id,
            "pending sequence 4 thread evidence differs");
    require(event->speaker_kind == "human"
                && event->speaker_id == expected_human_speaker_id,
            "pending sequence 4 speaker differs");

    const auto head = thread_store.find(thread_alias);
    require(head.has_value()
                && head->revision == expected_thread_revision
                && head->head_task_id == expected_task_id,
            "preferred head differs from sequence 4 evidence");

    const auto grant = authority_store.find_grant(expected_grant_id);
    require(grant.has_value()
                && grant->state == LocalGooseDialogueResponderGrantState::activated
                && grant->activated_lease_id
                    == std::optional<std::string>{expected_lease_id},
            "expected activated authority grant differs");

    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value(), "expected authority lease is missing");
    require(lease->state == LocalGooseDialogueResponderLeaseState::active,
            "authority lease is not active");
    require(lease->thread_alias == thread_alias
                && lease->speaker_kind == "system"
                && lease->speaker_id == responder_speaker_id,
            "authority lease actor/thread differs");
    require(lease->allowed_message_kinds
                == local_goose_dialogue_responder_message_intervention,
            "authority lease message-kind envelope differs");
    require(lease->max_system_turns == 1 && lease->turns_committed == 0,
            "authority lease turn accounting differs");
    const auto now = now_ms();
    require(now >= lease->issued_at_ms && now < lease->expires_at_ms,
            "authority lease is outside its validity window");
    require(now >= lease->next_eligible_at_ms,
            "authority lease is not currently eligible");
    return *lease;
}

void require_prebind_state(
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store,
    const LocalGooseDialogueResponderLease& lease)
{
    const auto now = now_ms();
    require(lease.expires_at_ms - now >= minimum_remaining_lease_ms,
            "less than five minutes remain on authority lease; refusing bind");

    const auto decision = authority_store.find_decision(expected_decision_id);
    require(decision.has_value(), "expected prepared decision is missing");
    require_decision_identity(*decision);
    require(decision->state == LocalGooseDialogueResponderDecisionState::prepared,
            "authority decision is not exactly prepared");
    require(!decision->intent_id && !decision->bound_at_ms,
            "prepared authority decision unexpectedly has intent evidence");
    const auto event_decision = authority_store.find_decision_for_event(
        consumer_id, expected_event_id);
    require(event_decision.has_value()
                && event_decision->decision_id == expected_decision_id,
            "sequence 4 decision lookup differs");
    require(responder_store.intents_in_state(
                LocalGooseDialogueResponderIntentState::prepared, 1024)
                .empty(),
            "prepared responder intent already exists before bind");
    require(!responder_store.find_disposition_for_event(
                consumer_id, expected_event_id),
            "sequence 4 disposition already exists before bind");
}

LocalGooseDialogueResponderIntent require_bound_state(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = authority_store.find_decision(expected_decision_id);
    require(decision.has_value(), "bound decision is missing");
    require_decision_identity(*decision);
    require(decision->state
                == LocalGooseDialogueResponderDecisionState::intent_bound,
            "authority decision is not intent-bound");
    require(decision->intent_id.has_value() && decision->bound_at_ms.has_value(),
            "intent-bound decision lacks durable binding evidence");

    const auto intent = responder_store.find_intent(*decision->intent_id);
    require(intent.has_value(), "bound responder intent is missing");
    require(intent->state == LocalGooseDialogueResponderIntentState::prepared,
            "bound responder intent is not prepared");
    require(intent->lease_id == expected_lease_id
                && intent->thread_alias == thread_alias,
            "bound responder intent lease/thread differs");
    require(intent->completion_sequence == expected_sequence
                && intent->event_id == expected_event_id
                && intent->result_sha256 == expected_result_sha256,
            "bound responder intent completion evidence differs");
    require(intent->triggering_task_id == expected_task_id
                && intent->expected_thread_revision == expected_thread_revision
                && intent->expected_head_task_id == expected_task_id,
            "bound responder intent preferred-head evidence differs");
    require(intent->speaker_kind == "system"
                && intent->speaker_id == responder_speaker_id,
            "bound responder intent actor differs");
    require(intent->message_kind == decision_message_kind
                && intent->message == decision_message,
            "bound responder intent message differs");
    require(intent->submitted_task_id.empty()
                && !intent->submitted_thread_revision
                && !intent->committed_at_ms
                && !intent->completed_at_ms,
            "bind unexpectedly created Task/submission evidence");

    const auto disposition = responder_store.find_disposition_for_event(
        consumer_id, expected_event_id);
    require(disposition.has_value(), "bound intervention disposition is missing");
    require(disposition->kind
                == LocalGooseDialogueResponderDispositionKind::intervention
                && disposition->state
                    == LocalGooseDialogueResponderDispositionState::intervention_bound,
            "sequence 4 disposition is not intervention-bound");
    require(disposition->authority_source == intervention_authority_source,
            "sequence 4 disposition authority source differs");
    require(disposition->intent_id
                == std::optional<std::string>{intent->intent_id}
                && disposition->bound_at_ms.has_value(),
            "sequence 4 disposition intent binding differs");
    require(completion_store.consumer_last_sequence(consumer_id)
                == std::optional<std::uint64_t>{3},
            "bind unexpectedly ACKed sequence 4");
    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease && lease->turns_committed == 0,
            "bind unexpectedly consumed a lease turn");
    return *intent;
}

void print_status(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = authority_store.find_decision(expected_decision_id);
    require(decision.has_value(), "expected authority decision is missing");
    require_decision_identity(*decision);
    const auto lease = responder_store.find_lease(expected_lease_id);

    std::cout << "AUTHORITY_ENABLED_FOR_ONESHOT=yes\n";
    std::cout << "AUTHORITY_SOURCE=" << authority_source << '\n';
    std::cout << "RESPONDER_CURSOR="
              << *completion_store.consumer_last_sequence(consumer_id) << '\n';
    std::cout << "NEXT_SEQUENCE=" << expected_sequence << '\n';
    std::cout << "GRANT_ID=" << expected_grant_id << '\n';
    std::cout << "LEASE_ID=" << expected_lease_id << '\n';
    std::cout << "LEASE_TURNS_COMMITTED=" << lease->turns_committed << '\n';
    std::cout << "LEASE_EXPIRES_AT_MS=" << lease->expires_at_ms << '\n';
    std::cout << "DECISION_ID=" << decision->decision_id << '\n';
    std::cout << "DECISION_MESSAGE_KIND=" << decision_message_kind << '\n';
    std::cout << "DECISION_MESSAGE=" << decision_message << '\n';

    if (decision->state == LocalGooseDialogueResponderDecisionState::prepared) {
        require(!decision->intent_id && !decision->bound_at_ms,
                "prepared decision unexpectedly has binding evidence");
        std::cout << "DECISION_STATE=prepared\n";
        std::cout << "BOUND_INTENT_ID=none\n";
        std::cout << "INTENT_STATE=absent\n";
        std::cout << "SEQUENCE4_DISPOSITION=absent\n";
    } else if (decision->state
               == LocalGooseDialogueResponderDecisionState::intent_bound) {
        const auto intent = require_bound_state(
            completion_store, responder_store, authority_store);
        std::cout << "DECISION_STATE=intent_bound\n";
        std::cout << "BOUND_INTENT_ID=" << intent.intent_id << '\n';
        std::cout << "INTENT_STATE=prepared\n";
        std::cout << "INTENT_SUBMITTED_TASK_ID=none\n";
        std::cout << "SEQUENCE4_DISPOSITION=intervention_bound\n";
    } else {
        fail("authority decision is in an unexpected state");
    }
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
        LocalGooseDialogueResponderAuthorityServiceConfig config;
        config.authority_enabled = true;
        config.authority_source = authority_source;
        config.consumer_id = consumer_id;
        LocalGooseDialogueResponderAuthorityService authority(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, now_ms);

        const auto lease = require_canonical_checkpoint(
            completion_store, thread_store, responder_store, authority_store);

        if (options.command == "bind-decision") {
            require_prebind_state(responder_store, authority_store, lease);
            const auto result = authority.bind_decision(expected_decision_id);
            require(result.result
                        == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                        && result.decision && result.intent,
                    "explicit first authority decision bind failed");
            require(result.decision->decision_id == expected_decision_id,
                    "bound authority decision identity differs");
            const auto intent = require_bound_state(
                completion_store, responder_store, authority_store);
            require(intent.intent_id == result.intent->intent_id,
                    "bound authority result intent differs from durable intent");

            std::cout << "ACTION=bind-decision\n";
            std::cout << "DECISION_ID=" << expected_decision_id << '\n';
            std::cout << "DECISION_STATE=intent_bound\n";
            std::cout << "BOUND_INTENT_ID=" << intent.intent_id << '\n';
            std::cout << "INTENT_STATE=prepared\n";
            std::cout << "INTENT_SUBMITTED_TASK_ID=none\n";
            std::cout << "SEQUENCE4_DISPOSITION=intervention_bound\n";
            std::cout << "LEASE_TURNS_COMMITTED=0\n";
            std::cout << "SEQUENCE4_ACK=no\n";
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DECISION_BIND=PASS\n";
            return 0;
        }

        print_status(completion_store, responder_store, authority_store);
        std::cout
            << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DECISION_BIND_INSPECT=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR=" << error.what() << '\n';
        return 1;
    }
}
