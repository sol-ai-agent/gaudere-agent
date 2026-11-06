#ifndef GAUDERE_AGENT_TESTS_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_FIXTURE_HPP
#define GAUDERE_AGENT_TESTS_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_FIXTURE_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderRetryAdmission.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <gaudere/work/TaskStore.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace gaudere_agent;
using Json = nlohmann::json;

std::string v3_id(const char c)
{
    return std::string{local_goose_dialogue_v3_task_prefix}
        + std::string(64, c);
}

std::string completion_event_id(
    const std::string& thread_alias,
    const std::uint64_t thread_revision,
    const std::string& task_id,
    const std::string& result_sha256)
{
    const Json identity{
        {"result_sha256", result_sha256},
        {"schema", "gaudere.cognition.local-goose-dialogue.completion.identity.v1"},
        {"task_id", task_id},
        {"thread_alias", thread_alias},
        {"thread_revision", thread_revision}
    };
    return std::string{local_goose_dialogue_completion_event_prefix}
        + sha256_hex(identity.dump());
}

struct TemporaryFile {
    explicit TemporaryFile(const std::string& label)
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-retry-admission-" + label + "-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
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

class FakeTaskStore final : public gaudere::work::TaskStore {
public:
    [[nodiscard]] std::optional<gaudere::work::Task> find(
        const std::string& id) const override
    {
        if (trigger && trigger->id == id) return trigger;
        if (submission && submission->id == id) return submission;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<gaudere::work::Task> find_by_idempotency_key(
        const std::string& key) const override
    {
        if (submission && submission->idempotency_key == key) return submission;
        return std::nullopt;
    }

    [[nodiscard]] std::optional<gaudere::work::Task> find_pending_for(
        const std::vector<std::string>&) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::vector<gaudere::work::Task> leased_with_expired_lease(
        gaudere::work::TimePoint) const override
    {
        return {};
    }

    [[nodiscard]] std::optional<gaudere::work::TimePoint>
    next_lease_expiry() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] bool has_active() const override { return false; }

    void save(const gaudere::work::Task& task) override
    {
        submission = task;
    }

    std::optional<gaudere::work::Task> trigger;
    std::optional<gaudere::work::Task> submission;
};

struct Fixture {
    Fixture()
        : responder_file("responder"),
          thread_file("thread"),
          completion_file("completion"),
          retry_file("retry"),
          responder(responder_file.path.string()),
          authority(
              responder_file.path.string(),
              LocalGooseDialogueResponderAuthorityStoreOpenMode::allow_schema_upgrade),
          thread(thread_file.path.string()),
          completion(completion_file.path.string()),
          retry(retry_file.path.string())
    {
        const auto root = v3_id('a');
        LocalGooseDialogueThreadHead head{"main", 0, root, root};
        assert(thread.seed(head).result
            == LocalGooseDialogueThreadStoreResult::accepted);
        for (std::uint64_t revision = 1; revision <= 3; ++revision) {
            const auto replacement = LocalGooseDialogueThreadHead{
                "main", revision, root,
                v3_id(static_cast<char>('a' + revision))};
            assert(thread.replace(head, replacement).result
                == LocalGooseDialogueThreadStoreResult::accepted);
            head = replacement;
        }

        for (std::uint64_t revision = 0; revision <= 3; ++revision) {
            const auto task_id = revision == 0
                ? root : v3_id(static_cast<char>('a' + revision));
            const auto result_sha = std::string(
                64, static_cast<char>('1' + revision));
            LocalGooseDialogueCompletionEvent event;
            event.thread_alias = "main";
            event.thread_revision = revision;
            event.task_id = task_id;
            event.root_task_id = root;
            event.turn_index = revision;
            event.request_id = "fixture-request-" + std::to_string(revision);
            event.speaker_kind = revision == 3 ? "human" : "system";
            event.speaker_id = revision == 3 ? "bertrand" : "sol";
            event.message_kind = revision == 3 ? "dialogue" : "observation";
            event.result_sha256 = result_sha;
            event.response = "fixture response";
            event.observed_completed_at_ms = 1000 + static_cast<std::int64_t>(revision);
            event.event_id = completion_event_id(
                event.thread_alias, revision, task_id, result_sha);
            const auto write = completion.append_for_revision(event);
            assert(write.result == LocalGooseDialogueCompletionFeedResult::accepted);
            assert(write.event);
            if (revision < 3) {
                assert(completion.acknowledge(
                    consumer_id, write.event->sequence).result
                    == LocalGooseDialogueCompletionFeedResult::accepted);
            } else {
                trigger_event = *write.event;
            }
        }
        assert(trigger_event.sequence == 4);
        assert(completion.consumer_last_sequence(consumer_id)
            == std::optional<std::uint64_t>{3});

        gaudere::work::Task trigger;
        trigger.id = trigger_event.task_id;
        tasks.trigger = trigger;

        LocalGooseDialogueResponderLease lease;
        lease.lease_id = historical_lease_id;
        lease.thread_alias = "main";
        lease.speaker_kind = "system";
        lease.speaker_id = "sol";
        lease.allowed_message_kinds =
            local_goose_dialogue_responder_message_intervention;
        lease.purpose = "Generation-1 retry admission fixture";
        lease.max_system_turns = 1;
        lease.issued_at_ms = 1000;
        lease.expires_at_ms = 9000;
        lease.min_interval_ms = 0;
        lease.next_eligible_at_ms = 1000;
        assert(responder.create_lease(lease).result
            == LocalGooseDialogueResponderStoreResult::accepted);

        grant = make_local_goose_dialogue_responder_grant(
            "retry-admission-historical-grant",
            "local-typed-operator-v1", "main", "sol",
            local_goose_dialogue_responder_message_intervention,
            lease.purpose, 1, 10000, 0, 900, 15000);
        assert(authority.prepare_grant(grant).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        const auto activated = authority.activate_grant(
            grant.grant_id, lease.lease_id, 1100);
        assert(activated.result == LocalGooseDialogueResponderStoreResult::accepted);
        grant = *activated.grant;

        decision = make_local_goose_dialogue_responder_decision(
            "retry-admission-historical-decision",
            "local-typed-operator-v1", grant.grant_id, lease.lease_id,
            consumer_id, trigger_event.sequence, trigger_event.event_id,
            trigger_event.result_sha256, "main", trigger_event.thread_revision,
            trigger_event.task_id, "human", "bertrand",
            trigger_event.thread_revision, trigger_event.task_id,
            "sol", "intervention", "Historical failed intervention.",
            5000, 1200);
        assert(authority.prepare_decision(decision).result
            == LocalGooseDialogueResponderStoreResult::accepted);

        intent = make_local_goose_dialogue_responder_intent(
            lease.lease_id, "main", trigger_event.sequence,
            trigger_event.event_id, trigger_event.result_sha256,
            trigger_event.task_id, trigger_event.thread_revision,
            trigger_event.task_id, "sol", "intervention",
            "Historical failed intervention.", 1300, 6000);
        assert(responder.prepare_intent(intent).result
            == LocalGooseDialogueResponderStoreResult::accepted);

        disposition = make_local_goose_dialogue_responder_disposition(
            consumer_id, "main", trigger_event.sequence,
            trigger_event.event_id, trigger_event.result_sha256,
            trigger_event.thread_revision, trigger_event.task_id,
            "human", "bertrand",
            LocalGooseDialogueResponderDispositionKind::intervention,
            "explicit-intervention-intent-v1", 1300);
        assert(responder.prepare_disposition(disposition).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        const auto bound_disposition = responder.bind_disposition_intent(
            disposition.disposition_id, intent.intent_id, 1400);
        assert(bound_disposition.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        disposition = *bound_disposition.disposition;

        const auto bound_decision = authority.bind_decision_intent(
            decision.decision_id, intent.intent_id, 1400);
        assert(bound_decision.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        decision = *bound_decision.decision;

        const auto closed_lease = responder.close_lease(
            lease.lease_id, LocalGooseDialogueResponderLeaseState::expired,
            "lease_expired", 10000);
        assert(closed_lease.result
            == LocalGooseDialogueResponderStoreResult::accepted);

        const auto expired_intent = responder.terminalize_intent(
            intent.intent_id, LocalGooseDialogueResponderIntentState::expired,
            "lease_or_intent_expired");
        assert(expired_intent.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        intent = *expired_intent.intent;

        const auto terminal_disposition = responder.terminalize_disposition(
            disposition.disposition_id, "lease_or_intent_expired", 10000);
        assert(terminal_disposition.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        disposition = *terminal_disposition.disposition;

        const auto expired_decision = authority.terminalize_decision(
            decision.decision_id,
            LocalGooseDialogueResponderDecisionState::expired,
            "bound_intent_expired_before_dispatch", 10000);
        assert(expired_decision.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        decision = *expired_decision.decision;

        request.request_id = "retry-admission-generation-1";
        request.consumer_id = consumer_id;
        request.historical_grant_id = grant.grant_id;
        request.historical_lease_id = lease.lease_id;
        request.historical_decision_id = decision.decision_id;
        request.historical_intent_id = intent.intent_id;
        request.historical_disposition_id = disposition.disposition_id;
    }

    LocalGooseDialogueResponderRetryAdmissionService service()
    {
        return LocalGooseDialogueResponderRetryAdmissionService(
            completion, thread, responder, authority, tasks, retry,
            [this] { return now_ms; });
    }

    static constexpr const char* consumer_id = "system-responder-v1";
    static constexpr const char* historical_lease_id = "retry-admission-lease-v1";

    TemporaryFile responder_file;
    TemporaryFile thread_file;
    TemporaryFile completion_file;
    TemporaryFile retry_file;
    LocalGooseDialogueResponderStore responder;
    LocalGooseDialogueResponderAuthorityStore authority;
    LocalGooseDialogueThreadStore thread;
    LocalGooseDialogueCompletionFeedStore completion;
    LocalGooseDialogueResponderRetryStore retry;
    FakeTaskStore tasks;
    std::int64_t now_ms = 30000;
    LocalGooseDialogueCompletionEvent trigger_event;
    LocalGooseDialogueResponderGrant grant;
    LocalGooseDialogueResponderDecision decision;
    LocalGooseDialogueResponderIntent intent;
    LocalGooseDialogueResponderDisposition disposition;
    LocalGooseDialogueResponderRetryAdmissionRequest request;
};

} // namespace

#endif
