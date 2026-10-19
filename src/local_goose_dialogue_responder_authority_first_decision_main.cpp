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
constexpr const char* grant_request_id =
    "first-production-responder-authority-grant-v1";
constexpr const char* expected_grant_id =
    "cognition.local-goose-dialogue.responder-grant.v1:54f5770768e55b5bb0ddf0289c2a42555e0b8fbef6675726c4bc23b853cb2b5a";
constexpr const char* expected_lease_id =
    "authority-lease-v1-e1a58d3a65a1771c8b04b5e41d6eaeb571a7070975c73d69b2117725f00fbf63";
constexpr const char* responder_speaker_id = "sol";
constexpr const char* grant_purpose =
    "First explicit durable responder decision-authority grant for production sequence 4";
constexpr const char* decision_request_id =
    "first-production-responder-authority-decision-seq4-v1";
constexpr const char* decision_message_kind = "intervention";
constexpr const char* decision_message =
    "Sol reprend le dialogue depuis le point de continuite valide.";
constexpr std::int64_t decision_intent_ttl_ms = 30LL * 60 * 1000;
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
constexpr std::uint64_t expected_max_system_turns = 1;
constexpr std::int64_t expected_authority_ttl_ms = 2LL * 60 * 60 * 1000;
constexpr std::int64_t expected_min_interval_ms = 0;

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
            << " <inspect|prepare-decision>"
            << " --state PATH --thread-sidecar PATH"
            << " --completion-sidecar PATH --responder-sidecar PATH\n";
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing command");

    Options out;
    out.command = argv[1];
    if (out.command != "inspect" && out.command != "prepare-decision") {
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

void require_grant_and_lease(
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto grant = authority_store.find_grant(expected_grant_id);
    require(grant.has_value(), "expected activated grant is missing");
    require(grant->request_id == grant_request_id,
            "authority grant request identity differs");
    require(grant->authority_source == authority_source,
            "authority grant source differs");
    require(grant->thread_alias == thread_alias,
            "authority grant thread differs");
    require(grant->speaker_kind == "system"
                && grant->speaker_id == responder_speaker_id,
            "authority grant responder identity differs");
    require(grant->allowed_message_kinds
                == local_goose_dialogue_responder_message_intervention,
            "authority grant message-kind envelope differs");
    require(grant->purpose == grant_purpose,
            "authority grant purpose differs");
    require(grant->max_system_turns == expected_max_system_turns,
            "authority grant turn bound differs");
    require(grant->max_lease_ttl_ms == expected_authority_ttl_ms,
            "authority grant lease TTL bound differs");
    require(grant->min_interval_ms == expected_min_interval_ms,
            "authority grant minimum interval differs");
    require(grant->state == LocalGooseDialogueResponderGrantState::activated,
            "authority grant is not activated");
    require(grant->activated_lease_id
                == std::optional<std::string>{expected_lease_id},
            "authority grant linked lease differs");

    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value(), "expected authority lease is missing");
    require(lease->state == LocalGooseDialogueResponderLeaseState::active,
            "authority lease is not active");
    require(lease->thread_alias == thread_alias,
            "authority lease thread differs");
    require(lease->speaker_kind == "system"
                && lease->speaker_id == responder_speaker_id,
            "authority lease responder identity differs");
    require(lease->allowed_message_kinds
                == local_goose_dialogue_responder_message_intervention,
            "authority lease message-kind envelope differs");
    require(lease->max_system_turns == expected_max_system_turns
                && lease->turns_committed == 0,
            "authority lease turn accounting differs");
    const auto now = now_ms();
    require(now >= lease->issued_at_ms && now < lease->expires_at_ms,
            "authority lease is not currently within its validity window");
    require(now >= lease->next_eligible_at_ms,
            "authority lease is not currently eligible");
}

void require_exact_checkpoint(
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
            "responder consumer cursor is not exactly 3");

    const auto event = completion_store.next_for_consumer(consumer_id);
    require(event.has_value(), "pending sequence 4 event is missing");
    require(event->sequence == expected_sequence,
            "pending completion sequence differs");
    require(event->event_id == expected_event_id,
            "pending completion event identity differs");
    require(event->result_sha256 == expected_result_sha256,
            "pending completion result hash differs");
    require(event->thread_alias == thread_alias,
            "pending completion thread alias differs");
    require(event->thread_revision == expected_thread_revision,
            "pending completion thread revision differs");
    require(event->task_id == expected_task_id,
            "pending completion Task identity differs");
    require(event->speaker_kind == "human"
                && event->speaker_id == expected_human_speaker_id,
            "pending completion is not the exact expected human event");

    const auto head = thread_store.find(thread_alias);
    require(head.has_value(), "preferred head is missing");
    require(head->revision == expected_thread_revision
                && head->head_task_id == expected_task_id,
            "preferred head differs from exact sequence 4 evidence");

    require(!responder_store.find_disposition_for_event(
                consumer_id, expected_event_id),
            "sequence 4 already has a responder disposition");
    require(responder_store.intents_in_state(
                LocalGooseDialogueResponderIntentState::prepared, 1024)
                .empty(),
            "production unexpectedly contains a prepared responder intent");
    require_grant_and_lease(responder_store, authority_store);
}

void require_decision_envelope(
    const LocalGooseDialogueResponderDecision& decision)
{
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
            "authority decision completion sequence differs");
    require(decision.event_id == expected_event_id,
            "authority decision event identity differs");
    require(decision.result_sha256 == expected_result_sha256,
            "authority decision result hash differs");
    require(decision.thread_alias == thread_alias
                && decision.thread_revision == expected_thread_revision,
            "authority decision thread evidence differs");
    require(decision.task_id == expected_task_id,
            "authority decision Task evidence differs");
    require(decision.observed_speaker_kind == "human"
                && decision.observed_speaker_id == expected_human_speaker_id,
            "authority decision observed speaker differs");
    require(decision.expected_head_revision == expected_thread_revision
                && decision.expected_head_task_id == expected_task_id,
            "authority decision preferred-head evidence differs");
    require(decision.responder_speaker_kind == "system"
                && decision.responder_speaker_id == responder_speaker_id,
            "authority decision responder identity differs");
    require(decision.message_kind == decision_message_kind,
            "authority decision message kind differs");
    require(decision.message == decision_message,
            "authority decision message bytes differ");
    require(decision.intent_ttl_ms == decision_intent_ttl_ms,
            "authority decision intent TTL differs");
    require(decision.state == LocalGooseDialogueResponderDecisionState::prepared,
            "authority decision is not prepared");
    require(!decision.intent_id && !decision.bound_at_ms,
            "prepared authority decision unexpectedly has an intent binding");
}

void print_status(
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = authority_store.find_decision_for_event(
        consumer_id, expected_event_id);
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
    std::cout << "DECISION_MESSAGE_KIND=" << decision_message_kind << '\n';
    std::cout << "DECISION_MESSAGE=" << decision_message << '\n';
    std::cout << "DECISION_INTENT_TTL_MS=" << decision_intent_ttl_ms << '\n';
    if (decision) {
        require_decision_envelope(*decision);
        std::cout << "DECISION_STATE=prepared\n";
        std::cout << "DECISION_ID=" << decision->decision_id << '\n';
        std::cout << "BOUND_INTENT_ID=none\n";
    } else {
        std::cout << "DECISION_STATE=absent\n";
        std::cout << "DECISION_ID=none\n";
        std::cout << "BOUND_INTENT_ID=none\n";
    }
    std::cout << "PREPARED_INTENT_COUNT="
              << responder_store.intents_in_state(
                     LocalGooseDialogueResponderIntentState::prepared, 1024)
                     .size()
              << '\n';
    std::cout << "SEQUENCE4_DISPOSITION="
              << (responder_store.find_disposition_for_event(
                      consumer_id, expected_event_id)
                      ? "present" : "absent")
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
        LocalGooseDialogueResponderAuthorityServiceConfig config;
        config.authority_enabled = true;
        config.authority_source = authority_source;
        config.consumer_id = consumer_id;
        LocalGooseDialogueResponderAuthorityService authority(
            completion_store, thread_store, responder_store, authority_store,
            dispatcher, config, now_ms);

        require_exact_checkpoint(
            completion_store, thread_store, responder_store, authority_store);

        if (options.command == "prepare-decision") {
            require(!authority_store.find_decision_for_event(
                        consumer_id, expected_event_id),
                    "authority decision for sequence 4 already exists");

            LocalGooseDialogueResponderDecisionPrepareRequest request;
            request.request_id = decision_request_id;
            request.grant_id = expected_grant_id;
            request.lease_id = expected_lease_id;
            request.consumer_id = consumer_id;
            request.completion_sequence = expected_sequence;
            request.event_id = expected_event_id;
            request.result_sha256 = expected_result_sha256;
            request.thread_alias = thread_alias;
            request.thread_revision = expected_thread_revision;
            request.task_id = expected_task_id;
            request.observed_speaker_kind = "human";
            request.observed_speaker_id = expected_human_speaker_id;
            request.expected_head_revision = expected_thread_revision;
            request.expected_head_task_id = expected_task_id;
            request.message_kind = decision_message_kind;
            request.message = decision_message;
            request.intent_ttl_ms = decision_intent_ttl_ms;

            const auto result = authority.prepare_decision(request);
            require(result.result
                        == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                        && result.decision,
                    "explicit first production authority decision preparation failed");
            require_decision_envelope(*result.decision);
            require(!result.intent,
                    "decision preparation unexpectedly returned an intent");
            require(responder_store.intents_in_state(
                        LocalGooseDialogueResponderIntentState::prepared, 1024)
                        .empty(),
                    "decision preparation unexpectedly created an intent");
            require(!responder_store.find_disposition_for_event(
                        consumer_id, expected_event_id),
                    "decision preparation unexpectedly created a disposition");
            require(completion_store.consumer_last_sequence(consumer_id)
                        == std::optional<std::uint64_t>{3},
                    "decision preparation unexpectedly ACKed sequence 4");
            const auto lease = responder_store.find_lease(expected_lease_id);
            require(lease && lease->turns_committed == 0,
                    "decision preparation unexpectedly consumed a lease turn");

            std::cout << "ACTION=prepare-decision\n";
            std::cout << "DECISION_ID=" << result.decision->decision_id << '\n';
            std::cout << "DECISION_STATE=prepared\n";
            std::cout << "DECISION_MESSAGE_KIND=" << decision_message_kind << '\n';
            std::cout << "DECISION_MESSAGE=" << decision_message << '\n';
            std::cout << "BOUND_INTENT_ID=none\n";
            std::cout << "LEASE_TURNS_COMMITTED=0\n";
            std::cout << "SEQUENCE4_ACK=no\n";
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DECISION_PREPARE=PASS\n";
            return 0;
        }

        print_status(completion_store, responder_store, authority_store);
        std::cout
            << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_DECISION_INSPECT=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere responder authority first decision: FAIL: "
            << error.what() << '\n';
        return 2;
    }
}
