#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

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
constexpr const char* expected_intent_id =
    "cognition.local-goose-dialogue.responder-intent.v1:f82c3df882b60babea7d88a36589aad57d1f7b0b2b9ec4d208e8be8d01b71fae";
constexpr const char* expected_intent_request_id =
    "responder-v1-f82c3df882b60babea7d88a36589aad57d1f7b0b2b9ec4d208e8be8d01b71fae";
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
constexpr const char* responder_terminal_reason = "lease_or_intent_expired";
constexpr const char* lease_terminal_reason = "lease_expired";
constexpr const char* decision_terminal_reason =
    "bound_intent_expired_before_dispatch";
constexpr const char* model_sha256 =
    "85a896a047553e842f25297ee5b031d64ff30147d9c4af17b1e4b394cd1fab87";
constexpr std::uint64_t expected_sequence = 4;
constexpr std::uint64_t expected_thread_revision = 3;

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
                  << " <inspect|recover-expired-bind>"
                  << " --state PATH --thread-sidecar PATH"
                  << " --completion-sidecar PATH --responder-sidecar PATH\n";
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing command");

    Options out;
    out.command = argv[1];
    if (out.command != "inspect" && out.command != "recover-expired-bind") {
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

std::string expected_submission_idempotency_key()
{
    return std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(expected_intent_request_id);
}

void require_no_submission_task(gaudere::work::TaskStore& task_store)
{
    require(!task_store.find_by_idempotency_key(
                expected_submission_idempotency_key()),
            "expired bound intent already has a durable submission Task");
}

void require_pending_sequence(
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store)
{
    require(completion_store.consumer_last_sequence(consumer_id)
                == std::optional<std::uint64_t>{3},
            "responder cursor is not exactly 3");
    const auto event = completion_store.next_for_consumer(consumer_id);
    require(event.has_value()
                && event->sequence == expected_sequence
                && event->event_id == expected_event_id
                && event->result_sha256 == expected_result_sha256
                && event->thread_alias == thread_alias
                && event->thread_revision == expected_thread_revision
                && event->task_id == expected_trigger_task_id
                && event->speaker_kind == "human"
                && event->speaker_id == expected_human_speaker_id,
            "pending sequence 4 evidence differs");
    const auto head = thread_store.find(thread_alias);
    require(head.has_value()
                && head->revision == expected_thread_revision
                && head->head_task_id == expected_trigger_task_id,
            "preferred head differs from pending sequence 4");
    require(task_store.find(expected_trigger_task_id).has_value(),
            "sequence 4 predecessor Task is missing");
}

LocalGooseDialogueResponderDecision require_decision_identity(
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = authority_store.find_decision(expected_decision_id);
    require(decision.has_value(), "expected authority decision is missing");
    require(decision->request_id == decision_request_id
                && decision->authority_source == authority_source,
            "authority decision provenance differs");
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
                && decision->thread_revision == expected_thread_revision
                && decision->task_id == expected_trigger_task_id,
            "authority decision thread evidence differs");
    require(decision->observed_speaker_kind == "human"
                && decision->observed_speaker_id == expected_human_speaker_id,
            "authority decision observed speaker differs");
    require(decision->expected_head_revision == expected_thread_revision
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
    return *decision;
}

LocalGooseDialogueResponderIntent require_intent_identity(
    LocalGooseDialogueResponderStore& responder_store)
{
    const auto intent = responder_store.find_intent(expected_intent_id);
    require(intent.has_value(), "expected responder intent is missing");
    require(intent->request_id == expected_intent_request_id,
            "responder intent request identity differs");
    require(intent->lease_id == expected_lease_id
                && intent->thread_alias == thread_alias,
            "responder intent lease/thread differs");
    require(intent->completion_sequence == expected_sequence
                && intent->event_id == expected_event_id
                && intent->result_sha256 == expected_result_sha256,
            "responder intent trigger evidence differs");
    require(intent->triggering_task_id == expected_trigger_task_id
                && intent->expected_thread_revision == expected_thread_revision
                && intent->expected_head_task_id == expected_trigger_task_id,
            "responder intent preferred-head evidence differs");
    require(intent->speaker_kind == "system"
                && intent->speaker_id == responder_speaker_id,
            "responder intent actor differs");
    require(intent->message_kind == decision_message_kind
                && intent->message == decision_message,
            "responder intent message differs");
    require(intent->submitted_task_id.empty()
                && !intent->submitted_thread_revision
                && !intent->committed_at_ms
                && !intent->completed_at_ms,
            "expired bound intent unexpectedly has submission evidence");
    return *intent;
}

LocalGooseDialogueResponderDisposition require_disposition_identity(
    LocalGooseDialogueResponderStore& responder_store)
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
    require(disposition->intent_id
                == std::optional<std::string>{expected_intent_id}
                && disposition->bound_at_ms.has_value(),
            "sequence 4 disposition intent binding differs");
    return *disposition;
}

void require_static_authority_identity(
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    require(authority_store.schema_version()
                == local_goose_dialogue_responder_authority_sidecar_schema,
            "responder authority schema is not exactly v3");
    const auto grant = authority_store.find_grant(expected_grant_id);
    require(grant.has_value()
                && grant->state == LocalGooseDialogueResponderGrantState::activated
                && grant->authority_source == authority_source
                && grant->activated_lease_id
                    == std::optional<std::string>{expected_lease_id}
                && grant->thread_alias == thread_alias
                && grant->speaker_kind == "system"
                && grant->speaker_id == responder_speaker_id
                && grant->max_system_turns == 1,
            "historical activated authority grant differs");
    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value()
                && lease->thread_alias == thread_alias
                && lease->speaker_kind == "system"
                && lease->speaker_id == responder_speaker_id
                && lease->max_system_turns == 1
                && lease->turns_committed == 0,
            "historical authority lease identity/accounting differs");
}

void require_pending_recovery_state(
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    const auto decision = require_decision_identity(authority_store);
    require(decision.state
                == LocalGooseDialogueResponderDecisionState::intent_bound,
            "authority decision is not the pending intent-bound decision");
    require(decision.terminal_reason.empty() && !decision.terminal_at_ms,
            "pending authority decision unexpectedly has terminal evidence");

    const auto lease = responder_store.find_lease(expected_lease_id);
    const auto intent = require_intent_identity(responder_store);
    const auto disposition = require_disposition_identity(responder_store);
    const auto now = now_ms();
    require(now >= lease->expires_at_ms,
            "authority lease has not expired yet; refusing recovery");
    require(now >= intent.expires_at_ms,
            "responder intent has not expired yet; refusing recovery");

    using LeaseState = LocalGooseDialogueResponderLeaseState;
    using IntentState = LocalGooseDialogueResponderIntentState;
    using DispositionState = LocalGooseDialogueResponderDispositionState;

    if (lease->state == LeaseState::active) {
        require(intent.state == IntentState::prepared
                    && disposition.state == DispositionState::intervention_bound,
                "active-but-expired lease has noncanonical recovery companions");
        return;
    }
    require(lease->state == LeaseState::expired
                && lease->terminal_reason == lease_terminal_reason
                && lease->terminal_at_ms.has_value(),
            "partially recovered lease differs");
    if (intent.state == IntentState::prepared) {
        require(disposition.state == DispositionState::intervention_bound,
                "prepared intent lost its bound intervention disposition");
        return;
    }
    require(intent.state == IntentState::expired
                && intent.terminal_reason == responder_terminal_reason,
            "partially recovered intent differs");
    require(disposition.state == DispositionState::intervention_bound
                || (disposition.state == DispositionState::manual_review
                    && disposition.terminal_reason == responder_terminal_reason
                    && disposition.terminal_at_ms.has_value()),
            "partially recovered disposition differs");
}

void require_recovered_responder_state(
    LocalGooseDialogueResponderStore& responder_store)
{
    const auto lease = responder_store.find_lease(expected_lease_id);
    require(lease.has_value()
                && lease->state == LocalGooseDialogueResponderLeaseState::expired
                && lease->turns_committed == 0
                && lease->terminal_reason == lease_terminal_reason
                && lease->terminal_at_ms.has_value()
                && *lease->terminal_at_ms >= lease->expires_at_ms,
            "authority lease was not durably normalized as expired");

    const auto intent = require_intent_identity(responder_store);
    require(intent.state == LocalGooseDialogueResponderIntentState::expired
                && intent.terminal_reason == responder_terminal_reason,
            "responder intent was not durably normalized as expired");

    const auto disposition = require_disposition_identity(responder_store);
    require(disposition.state
                == LocalGooseDialogueResponderDispositionState::manual_review
                && disposition.terminal_reason == responder_terminal_reason
                && disposition.terminal_at_ms.has_value(),
            "intervention disposition was not durably reconciled to manual review");
}

void require_fully_recovered_state(
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueCompletionFeedStore& completion_store,
    LocalGooseDialogueThreadStore& thread_store,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    require_pending_sequence(task_store, completion_store, thread_store);
    require_static_authority_identity(responder_store, authority_store);
    require_no_submission_task(task_store);
    require_recovered_responder_state(responder_store);

    const auto decision = require_decision_identity(authority_store);
    require(decision.state == LocalGooseDialogueResponderDecisionState::expired
                && decision.terminal_reason == decision_terminal_reason
                && decision.terminal_at_ms.has_value(),
            "authority decision was not durably terminalized as expired");
}

void print_pending_status(
    LocalGooseDialogueResponderStore& responder_store)
{
    const auto lease = responder_store.find_lease(expected_lease_id);
    const auto intent = responder_store.find_intent(expected_intent_id);
    const auto disposition = responder_store.find_disposition_for_event(
        consumer_id, expected_event_id);
    std::cout << "PHASE=expired_bound_pending_recovery\n";
    std::cout << "RESPONDER_CURSOR=3\n";
    std::cout << "NEXT_SEQUENCE=4\n";
    std::cout << "DECISION_STATE=intent_bound\n";
    std::cout << "INTENT_STATE="
              << (intent->state == LocalGooseDialogueResponderIntentState::prepared
                      ? "prepared" : "expired") << '\n';
    std::cout << "DISPOSITION_STATE="
              << (disposition->state
                          == LocalGooseDialogueResponderDispositionState::intervention_bound
                      ? "intervention_bound" : "manual_review") << '\n';
    std::cout << "LEASE_STATE="
              << (lease->state == LocalGooseDialogueResponderLeaseState::active
                      ? "active" : "expired") << '\n';
    std::cout << "LEASE_TURNS_COMMITTED=0\n";
    std::cout << "SEQUENCE4_ACK=no\n";
    std::cout << "SUBMISSION_TASK=absent\n";
}

void print_recovered_status()
{
    std::cout << "PHASE=expired_bound_recovered\n";
    std::cout << "RESPONDER_CURSOR=3\n";
    std::cout << "NEXT_SEQUENCE=4\n";
    std::cout << "DECISION_STATE=expired\n";
    std::cout << "DECISION_TERMINAL_REASON=" << decision_terminal_reason << '\n';
    std::cout << "INTENT_STATE=expired\n";
    std::cout << "INTENT_TERMINAL_REASON=" << responder_terminal_reason << '\n';
    std::cout << "DISPOSITION_STATE=manual_review\n";
    std::cout << "DISPOSITION_TERMINAL_REASON=" << responder_terminal_reason << '\n';
    std::cout << "LEASE_STATE=expired\n";
    std::cout << "LEASE_TURNS_COMMITTED=0\n";
    std::cout << "SEQUENCE4_ACK=no\n";
    std::cout << "HEAD_REVISION=3\n";
    std::cout << "SUBMISSION_TASK=absent\n";
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

        require_pending_sequence(task_store, completion_store, thread_store);
        require_static_authority_identity(responder_store, authority_store);
        require_no_submission_task(task_store);

        const auto decision = require_decision_identity(authority_store);
        if (options.command == "inspect") {
            if (decision.state
                == LocalGooseDialogueResponderDecisionState::intent_bound) {
                require_pending_recovery_state(responder_store, authority_store);
                print_pending_status(responder_store);
            } else if (decision.state
                       == LocalGooseDialogueResponderDecisionState::expired) {
                require_fully_recovered_state(
                    task_store, completion_store, thread_store,
                    responder_store, authority_store);
                print_recovered_status();
            } else {
                fail("authority decision is neither pending recovery nor recovered");
            }
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY_INSPECT=PASS\n";
            return 0;
        }

        if (decision.state == LocalGooseDialogueResponderDecisionState::expired) {
            require_fully_recovered_state(
                task_store, completion_store, thread_store,
                responder_store, authority_store);
            print_recovered_status();
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY=PASS\n";
            return 0;
        }

        require_pending_recovery_state(responder_store, authority_store);
        const auto dispatch = dispatcher.dispatch(expected_intent_id);
        require(dispatch.result == LocalGooseDialogueResponderDispatchCode::conflict,
                "expired bound dispatcher recovery did not fail closed: "
                    + dispatch.detail);
        require(!dispatch.task.has_value(),
                "expired bound dispatcher recovery returned a Task");
        require_recovered_responder_state(responder_store);
        require_no_submission_task(task_store);
        require_pending_sequence(task_store, completion_store, thread_store);

        const auto terminal = authority_store.terminalize_decision(
            expected_decision_id,
            LocalGooseDialogueResponderDecisionState::expired,
            decision_terminal_reason,
            now_ms());
        require(terminal.result == LocalGooseDialogueResponderStoreResult::accepted
                    || terminal.result == LocalGooseDialogueResponderStoreResult::duplicate,
                "authority decision expiry terminalization failed: "
                    + terminal.detail);

        require_fully_recovered_state(
            task_store, completion_store, thread_store,
            responder_store, authority_store);
        std::cout << "ACTION=recover-expired-bind\n";
        print_recovered_status();
        std::cout
            << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_EXPIRED_BIND_RECOVERY=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR=" << error.what() << '\n';
        return 2;
    }
}
