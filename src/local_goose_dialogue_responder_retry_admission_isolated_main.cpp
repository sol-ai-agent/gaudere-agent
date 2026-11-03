#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderRetryAdmission.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;

constexpr const char* consumer = "system-responder-v1";
constexpr const char* request_id =
    "first-production-responder-retry-generation1-admission-seq4-v1";
constexpr const char* event_id =
    "cognition.local-goose-dialogue.completion.v1:7297b1ad75c62c19a46780ab8cce874353d7fe95b260c504c844878a9d8db844";
constexpr const char* result_sha =
    "4fa4966a87986dd6d1a163c1a35d577f78cd0f2f08409acca431a6a97fc29935";
constexpr const char* task_id =
    "cognition.local-goose-dialogue.v3:63963768e5dc127014698116c09539fcf6cbc13019717151400775204652cbaa";
constexpr const char* grant_id =
    "cognition.local-goose-dialogue.responder-grant.v1:54f5770768e55b5bb0ddf0289c2a42555e0b8fbef6675726c4bc23b853cb2b5a";
constexpr const char* lease_id =
    "authority-lease-v1-e1a58d3a65a1771c8b04b5e41d6eaeb571a7070975c73d69b2117725f00fbf63";
constexpr const char* decision_id =
    "cognition.local-goose-dialogue.responder-decision.v1:5e8bfd1668346f209eaec91f5888a3a9532a5bf5677674d4c2001f499e924153";
constexpr const char* intent_id =
    "cognition.local-goose-dialogue.responder-intent.v1:f82c3df882b60babea7d88a36589aad57d1f7b0b2b9ec4d208e8be8d01b71fae";
constexpr const char* disposition_id =
    "cognition.local-goose-dialogue.responder-disposition.v1:72741315708884592db29c29c4e6c9d750b6e49f3b15a1bd1b61f9c1e618236b";

constexpr const char* state = "/rehearsal/state.db";
constexpr const char* thread = "/rehearsal/local-goose-dialogue-thread.db";
constexpr const char* completion = "/rehearsal/local-goose-dialogue-completion.db";
constexpr const char* responder = "/rehearsal/local-goose-dialogue-responder.db";
constexpr const char* retry = "/rehearsal/local-goose-dialogue-responder-retry.db";

void require(bool condition, const char* detail)
{
    if (!condition) throw std::runtime_error(detail);
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void require_snapshot_files()
{
    for (const char* path : {state, thread, completion, responder}) {
        const auto status = std::filesystem::symlink_status(path);
        require(std::filesystem::is_regular_file(status),
                "missing or unsafe rehearsal snapshot file");
    }
    const auto status = std::filesystem::symlink_status("/rehearsal");
    require(std::filesystem::is_directory(status),
            "rehearsal path is not a real directory");
}

struct Stores {
    gaudere::persistence::sqlite::TaskStore tasks{state};
    LocalGooseDialogueThreadStore threads{thread};
    LocalGooseDialogueCompletionFeedStore feed{completion};
    LocalGooseDialogueResponderStore responder_store{responder};
    LocalGooseDialogueResponderAuthorityStore authority{responder};
    std::unique_ptr<LocalGooseDialogueResponderRetryStore> retry_store;

    explicit Stores(const bool may_create_retry_sidecar)
    {
        const auto status = std::filesystem::symlink_status(retry);
        if (std::filesystem::is_regular_file(status)) {
            retry_store = std::make_unique<LocalGooseDialogueResponderRetryStore>(retry);
        } else if (status.type() == std::filesystem::file_type::not_found
                   && may_create_retry_sidecar) {
            retry_store = std::make_unique<LocalGooseDialogueResponderRetryStore>(retry);
        } else {
            require(status.type() == std::filesystem::file_type::not_found,
                    "retry sidecar path is unsafe");
        }
    }
};

void prove_unchanged_trigger(const Stores& stores)
{
    const auto cursor = stores.feed.consumer_last_sequence(consumer);
    require(cursor && *cursor == 3, "retry consumer cursor is not exactly 3");
    const auto event = stores.feed.next_for_consumer(consumer);
    require(event && event->sequence == 4 && event->event_id == event_id
            && event->result_sha256 == result_sha && event->thread_alias == "main"
            && event->thread_revision == 3 && event->task_id == task_id
            && event->speaker_kind == "human" && event->speaker_id == "bertrand",
            "sequence 4 trigger differs from recovered authority proof");
    const auto head = stores.threads.find("main");
    require(head && head->revision == 3 && head->head_task_id == task_id,
            "preferred head is no longer the sequence 4 trigger");
    require(stores.tasks.find(task_id).has_value(),
            "canonical trigger task is missing");
    const auto intent = stores.responder_store.find_intent(intent_id);
    require(intent && intent->state == LocalGooseDialogueResponderIntentState::expired
            && intent->terminal_reason == "lease_or_intent_expired"
            && intent->submitted_task_id.empty() && !intent->submitted_thread_revision,
            "historical intent is not expired without submission");
    const auto submission_key = std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(intent->request_id);
    require(!stores.tasks.find_by_idempotency_key(submission_key),
            "historical submission Task exists");
    const auto grant = stores.authority.find_grant(grant_id);
    const auto lease = stores.responder_store.find_lease(lease_id);
    const auto decision = stores.authority.find_decision(decision_id);
    const auto disposition = stores.responder_store.find_disposition(disposition_id);
    require(grant && grant->state == LocalGooseDialogueResponderGrantState::activated
            && grant->activated_lease_id == std::optional<std::string>{lease_id}
            && grant->expires_at_ms <= now_ms(),
            "historical grant is not bound to an expired window");
    require(lease && lease->state == LocalGooseDialogueResponderLeaseState::expired
            && lease->terminal_reason == "lease_expired"
            && lease->turns_committed == 0,
            "historical lease differs");
    require(decision && decision->state == LocalGooseDialogueResponderDecisionState::expired
            && decision->terminal_reason == "bound_intent_expired_before_dispatch"
            && decision->intent_id == std::optional<std::string>{intent_id},
            "historical decision differs");
    require(disposition
            && disposition->state == LocalGooseDialogueResponderDispositionState::manual_review
            && disposition->terminal_reason == "lease_or_intent_expired"
            && disposition->intent_id == std::optional<std::string>{intent_id},
            "historical disposition differs");
}

void prove_lineage(const LocalGooseDialogueResponderRetryLineage& lineage)
{
    require(lineage.request_id == request_id && lineage.consumer_id == consumer
            && lineage.completion_sequence == 4
            && lineage.event_id == event_id && lineage.result_sha256 == result_sha
            && lineage.thread_alias == "main" && lineage.thread_revision == 3
            && lineage.task_id == task_id
            && lineage.observed_speaker_kind == "human"
            && lineage.observed_speaker_id == "bertrand"
            && lineage.retry_generation == 1
            && lineage.historical_grant_id == grant_id
            && lineage.historical_lease_id == lease_id
            && lineage.historical_decision_id == decision_id
            && lineage.historical_intent_id == intent_id
            && lineage.historical_disposition_id == disposition_id
            && lineage.historical_lease_terminal_reason == "lease_expired"
            && lineage.historical_decision_terminal_reason
                == "bound_intent_expired_before_dispatch"
            && lineage.historical_intent_terminal_reason
                == "lease_or_intent_expired"
            && lineage.historical_disposition_terminal_reason
                == "lease_or_intent_expired"
            && valid_local_goose_dialogue_responder_retry_lineage(lineage),
            "retry lineage identity differs");
}

void print_state(const Stores& stores)
{
    prove_unchanged_trigger(stores);
    require(stores.authority.schema_version() == 3, "authority schema differs");
    if (stores.retry_store) {
        require(stores.retry_store->schema_version() == 1, "retry schema differs");
    }
    const auto existing = stores.retry_store
        ? stores.retry_store->find_for_event(consumer, event_id)
        : std::optional<LocalGooseDialogueResponderRetryLineage>{};
    if (existing) prove_lineage(*existing);
    std::cout << "RESPONDER_CURSOR=3\n"
              << "NEXT_SEQUENCE=4\n"
              << "HEAD_REVISION=3\n"
              << "SEQUENCE4_ACK=no\n"
              << "SUBMISSION_TASK=absent\n"
              << "DECISION_STATE=expired\n"
              << "INTENT_STATE=expired\n"
              << "DISPOSITION_STATE=manual_review\n"
              << "LEASE_STATE=expired\n"
              << "LEASE_TURNS_COMMITTED=0\n"
              << "LINEAGE_PRESENT=" << (existing ? "yes" : "no") << '\n';
    if (existing) std::cout << "LINEAGE_ID=" << existing->lineage_id << '\n';
    std::cout << "GAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED_INSPECT=PASS\n";
}

void admit(Stores& stores)
{
    prove_unchanged_trigger(stores);
    require(stores.retry_store != nullptr, "retry admission sidecar unavailable");
    const auto existing = stores.retry_store->find_for_event(consumer, event_id);
    if (existing) prove_lineage(*existing);
    const auto timestamp = existing ? existing->created_at_ms : now_ms();
    const LocalGooseDialogueResponderRetryAdmissionRequest request{
        request_id, consumer, grant_id, lease_id, decision_id, intent_id,
        disposition_id
    };
    LocalGooseDialogueResponderRetryAdmissionService service{
        stores.feed, stores.threads, stores.responder_store, stores.authority,
        stores.tasks, *stores.retry_store, [timestamp] { return timestamp; }
    };
    const auto write = service.prepare_lineage(request);
    using Result = LocalGooseDialogueResponderRetryAdmissionResult;
    require(write.result == Result::accepted || write.result == Result::duplicate,
            "retry admission rejected exact recovered lineage");
    require(write.lineage.has_value(), "retry admission yielded no durable lineage");
    prove_lineage(*write.lineage);
    const auto persisted = stores.retry_store->find_for_event(consumer, event_id);
    require(persisted.has_value()
            && persisted->lineage_id == write.lineage->lineage_id,
            "retry lineage is not durably persisted");
    prove_unchanged_trigger(stores);
    std::cout << "ADMISSION_RESULT="
              << (write.result == Result::accepted ? "accepted" : "duplicate")
              << "\nLINEAGE_ID=" << persisted->lineage_id
              << "\nGAUDERE_RESPONDER_RETRY_ADMISSION_ISOLATED=PASS\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string{argv[1]} == "--help") {
            std::cout << "Usage: retry-admission <inspect|admit-generation-1>"
                         " (hard-locked to /rehearsal snapshot)\n";
            return 0;
        }
        require(argc == 2, "exactly one command is required");
        const std::string command = argv[1];
        require(command == "inspect" || command == "admit-generation-1",
                "unsupported command");
        require_snapshot_files();
        Stores stores{command == "admit-generation-1"};
        if (command == "inspect") print_state(stores);
        else admit(stores);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RETRY_ADMISSION_ISOLATED=ABORTED: "
                  << error.what() << '\n';
        return 1;
    }
}
