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
constexpr const char* responder_speaker_id = "sol";
constexpr const char* grant_purpose =
    "First explicit durable responder decision-authority grant for production sequence 4";
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
constexpr std::uint64_t max_system_turns = 1;
constexpr std::int64_t authority_ttl_ms = 2LL * 60 * 60 * 1000;
constexpr std::int64_t min_interval_ms = 0;

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
            << " <inspect|prepare-grant|activate-grant>"
            << " --state PATH --thread-sidecar PATH"
            << " --completion-sidecar PATH --responder-sidecar PATH\n";
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing command");

    Options out;
    out.command = argv[1];
    if (out.command != "inspect" && out.command != "prepare-grant"
        && out.command != "activate-grant") {
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
    require(authority_store.decisions_in_state(
                LocalGooseDialogueResponderDecisionState::prepared, 1024)
                .empty(),
            "production unexpectedly contains a prepared authority decision");
    require(authority_store.decisions_in_state(
                LocalGooseDialogueResponderDecisionState::intent_bound, 1024)
                .empty(),
            "production unexpectedly contains an intent-bound authority decision");
}

void require_grant_envelope(
    const LocalGooseDialogueResponderGrant& grant)
{
    require(grant.request_id == grant_request_id,
            "authority grant request identity differs");
    require(grant.authority_source == authority_source,
            "authority grant source differs");
    require(grant.thread_alias == thread_alias,
            "authority grant thread differs");
    require(grant.speaker_kind == "system"
                && grant.speaker_id == responder_speaker_id,
            "authority grant responder identity differs");
    require(grant.allowed_message_kinds
                == local_goose_dialogue_responder_message_intervention,
            "authority grant message-kind envelope differs");
    require(grant.purpose == grant_purpose,
            "authority grant purpose differs");
    require(grant.max_system_turns == max_system_turns,
            "authority grant turn bound differs");
    require(grant.max_lease_ttl_ms == authority_ttl_ms,
            "authority grant lease TTL bound differs");
    require(grant.min_interval_ms == min_interval_ms,
            "authority grant minimum interval differs");
}

void print_status(
    const LocalGooseDialogueResponderAuthorityStatus& status,
    LocalGooseDialogueResponderStore& responder_store,
    LocalGooseDialogueResponderAuthorityStore& authority_store)
{
    std::size_t grant_count = 0;
    for (const auto state : {
             LocalGooseDialogueResponderGrantState::prepared,
             LocalGooseDialogueResponderGrantState::activated,
             LocalGooseDialogueResponderGrantState::revoked,
             LocalGooseDialogueResponderGrantState::expired,
             LocalGooseDialogueResponderGrantState::manual_review}) {
        grant_count += authority_store.grants_in_state(state, 1024).size();
    }
    std::size_t decision_count = 0;
    for (const auto state : {
             LocalGooseDialogueResponderDecisionState::prepared,
             LocalGooseDialogueResponderDecisionState::intent_bound,
             LocalGooseDialogueResponderDecisionState::completed,
             LocalGooseDialogueResponderDecisionState::conflict,
             LocalGooseDialogueResponderDecisionState::expired,
             LocalGooseDialogueResponderDecisionState::manual_review}) {
        decision_count += authority_store.decisions_in_state(state, 1024).size();
    }

    std::cout << "AUTHORITY_ENABLED_FOR_ONESHOT="
              << (status.authority_enabled ? "yes" : "no") << '\n';
    std::cout << "AUTHORITY_SOURCE=" << status.authority_source << '\n';
    std::cout << "RESPONDER_CURSOR="
              << (status.consumer_last_sequence
                      ? std::to_string(*status.consumer_last_sequence)
                      : std::string{"none"})
              << '\n';
    std::cout << "NEXT_SEQUENCE="
              << (status.next_event
                      ? std::to_string(status.next_event->sequence)
                      : std::string{"none"})
              << '\n';
    std::cout << "AUTHORITY_GRANT_COUNT=" << grant_count << '\n';
    std::cout << "AUTHORITY_DECISION_COUNT=" << decision_count << '\n';
    std::cout << "PREPARED_INTENT_COUNT="
              << responder_store.intents_in_state(
                     LocalGooseDialogueResponderIntentState::prepared, 1024)
                     .size()
              << '\n';
    if (status.prepared_grant) {
        require_grant_envelope(*status.prepared_grant);
        std::cout << "GRANT_STATE=prepared\n";
        std::cout << "GRANT_ID=" << status.prepared_grant->grant_id << '\n';
        std::cout << "LEASE_ID=none\n";
    } else if (status.activated_grant) {
        require_grant_envelope(*status.activated_grant);
        require(status.activated_grant->activated_lease_id.has_value(),
                "activated grant has no linked lease identity");
        const auto lease = responder_store.find_lease(
            *status.activated_grant->activated_lease_id);
        require(lease.has_value(), "activated grant lease is missing");
        require(lease->state == LocalGooseDialogueResponderLeaseState::active,
                "activated grant lease is not active");
        require(lease->turns_committed == 0
                    && lease->max_system_turns == max_system_turns,
                "activated grant lease turn accounting differs");
        require(lease->thread_alias == thread_alias
                    && lease->speaker_kind == "system"
                    && lease->speaker_id == responder_speaker_id
                    && lease->allowed_message_kinds
                        == local_goose_dialogue_responder_message_intervention,
                "activated grant lease envelope differs");
        std::cout << "GRANT_STATE=activated\n";
        std::cout << "GRANT_ID=" << status.activated_grant->grant_id << '\n';
        std::cout << "LEASE_ID=" << lease->lease_id << '\n';
        std::cout << "LEASE_STATE=active\n";
        std::cout << "LEASE_MAX_SYSTEM_TURNS=" << lease->max_system_turns << '\n';
        std::cout << "LEASE_TURNS_COMMITTED=" << lease->turns_committed << '\n';
        std::cout << "LEASE_EXPIRES_AT_MS=" << lease->expires_at_ms << '\n';
    } else {
        std::cout << "GRANT_STATE=absent\n";
        std::cout << "GRANT_ID=none\n";
        std::cout << "LEASE_ID=none\n";
    }
    std::cout << "STATUS_DETAIL=" << status.detail << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        gaudere::persistence::sqlite::TaskStore task_store(options.state);
        gaudere::work::Runtime runtime(task_store);
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

        if (options.command == "prepare-grant") {
            const auto status_before = authority.inspect(thread_alias);
            require(!status_before.prepared_grant
                        && !status_before.activated_grant
                        && !status_before.active_lease,
                    "authority grant or active lease already exists");
            require(authority_store.grants_in_state(
                        LocalGooseDialogueResponderGrantState::prepared, 1024)
                        .empty()
                        && authority_store.grants_in_state(
                               LocalGooseDialogueResponderGrantState::activated,
                               1024)
                               .empty(),
                    "authority grant prestate is not empty");

            LocalGooseDialogueResponderGrantPrepareRequest request;
            request.request_id = grant_request_id;
            request.thread_alias = thread_alias;
            request.speaker_id = responder_speaker_id;
            request.allowed_message_kinds =
                local_goose_dialogue_responder_message_intervention;
            request.purpose = grant_purpose;
            request.max_system_turns = max_system_turns;
            request.max_lease_ttl_ms = authority_ttl_ms;
            request.min_interval_ms = min_interval_ms;
            request.grant_ttl_ms = authority_ttl_ms;

            const auto result = authority.prepare_grant(request);
            require(result.result
                        == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                        && result.grant,
                    "explicit first production grant preparation failed");
            require(result.grant->state
                        == LocalGooseDialogueResponderGrantState::prepared,
                    "prepared first production grant state differs");
            require_grant_envelope(*result.grant);
            require(!responder_store.find_active_lease(thread_alias),
                    "grant preparation unexpectedly activated a lease");
            std::cout << "ACTION=prepare-grant\n";
            std::cout << "GRANT_ID=" << result.grant->grant_id << '\n';
            std::cout << "GRANT_STATE=prepared\n";
            std::cout << "LEASE_CREATED=no\n";
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_PREPARE=PASS\n";
            return 0;
        }

        if (options.command == "activate-grant") {
            const auto status_before = authority.inspect(thread_alias);
            require(status_before.prepared_grant.has_value(),
                    "expected prepared first production grant is missing");
            require(!status_before.activated_grant
                        && !status_before.active_lease,
                    "first production grant is already activated");
            require_grant_envelope(*status_before.prepared_grant);

            const auto result = authority.activate_grant(
                status_before.prepared_grant->grant_id);
            require(result.result
                        == LocalGooseDialogueResponderAuthorityServiceCode::accepted
                        && result.grant && result.lease,
                    "explicit first production grant activation failed");
            require(result.grant->state
                        == LocalGooseDialogueResponderGrantState::activated,
                    "activated first production grant state differs");
            require_grant_envelope(*result.grant);
            require(result.lease->state
                        == LocalGooseDialogueResponderLeaseState::active,
                    "first production authority lease is not active");
            require(result.lease->turns_committed == 0
                        && result.lease->max_system_turns == max_system_turns,
                    "first production authority lease turn bound differs");
            std::cout << "ACTION=activate-grant\n";
            std::cout << "GRANT_ID=" << result.grant->grant_id << '\n';
            std::cout << "GRANT_STATE=activated\n";
            std::cout << "LEASE_ID=" << result.lease->lease_id << '\n';
            std::cout << "LEASE_STATE=active\n";
            std::cout << "LEASE_MAX_SYSTEM_TURNS="
                      << result.lease->max_system_turns << '\n';
            std::cout << "LEASE_TURNS_COMMITTED="
                      << result.lease->turns_committed << '\n';
            std::cout
                << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_ACTIVATE=PASS\n";
            return 0;
        }

        const auto status = authority.inspect(thread_alias);
        std::cout << "ACTION=inspect\n";
        print_status(status, responder_store, authority_store);
        std::cout
            << "GAUDERE_DIALOGUE_RESPONDER_AUTHORITY_FIRST_GRANT_INSPECT=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere first responder authority grant helper: FAIL: "
            << error.what() << '\n';
        return 1;
    }
}
