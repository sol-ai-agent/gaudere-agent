#include "LocalGooseDialogueResponder.hpp"
#include "LocalGooseDialogueResponderAuthority.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace {

using namespace gaudere_agent;

struct TemporaryFile {
    TemporaryFile()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-bound-decision-expiry-"
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

std::string completion_id(const char c)
{
    return std::string{"cognition.local-goose-dialogue.completion.v1:"}
        + std::string(64, c);
}

std::string v3_id(const char c)
{
    return std::string{"cognition.local-goose-dialogue.v3:"}
        + std::string(64, c);
}

} // namespace

int main()
{
    TemporaryFile file;
    LocalGooseDialogueResponderStore responder(file.path.string());

    LocalGooseDialogueResponderLease lease;
    lease.lease_id = "bound-expiry-lease";
    lease.thread_alias = "main";
    lease.speaker_kind = "system";
    lease.speaker_id = "sol";
    lease.allowed_message_kinds =
        local_goose_dialogue_responder_message_intervention;
    lease.purpose = "Bound decision expiry proof";
    lease.max_system_turns = 1;
    lease.issued_at_ms = 1000;
    lease.expires_at_ms = 9000;
    lease.min_interval_ms = 0;
    lease.next_eligible_at_ms = 1000;
    assert(responder.create_lease(lease).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    LocalGooseDialogueResponderAuthorityStore authority(
        file.path.string(),
        LocalGooseDialogueResponderAuthorityStoreOpenMode::allow_schema_upgrade);
    assert(authority.schema_version()
        == local_goose_dialogue_responder_authority_sidecar_schema);

    const auto grant = make_local_goose_dialogue_responder_grant(
        "bound-expiry-grant-request", "local-typed-operator-v1",
        "main", "sol", local_goose_dialogue_responder_message_intervention,
        lease.purpose, 1, 10000, 0, 900, 20000);
    assert(authority.prepare_grant(grant).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    const auto activated = authority.activate_grant(
        grant.grant_id, lease.lease_id, 1100);
    assert(activated.result == LocalGooseDialogueResponderStoreResult::accepted);

    const auto decision = make_local_goose_dialogue_responder_decision(
        "bound-expiry-decision-request", "local-typed-operator-v1",
        grant.grant_id, lease.lease_id, "system-responder-v1", 4,
        completion_id('1'), std::string(64, '2'), "main", 3, v3_id('3'),
        "human", "bertrand", 3, v3_id('3'), "sol", "intervention",
        "Recovery proof message.", 5000, 1200);
    assert(authority.prepare_decision(decision).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto intent = make_local_goose_dialogue_responder_intent(
        lease.lease_id, "main", 4,
        completion_id('1'), std::string(64, '2'), v3_id('3'),
        3, v3_id('3'), "sol", "intervention", "Recovery proof message.",
        1300, 8000);
    assert(responder.prepare_intent(intent).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto disposition = make_local_goose_dialogue_responder_disposition(
        "system-responder-v1", "main", 4,
        completion_id('1'), std::string(64, '2'), 3, v3_id('3'),
        "human", "bertrand",
        LocalGooseDialogueResponderDispositionKind::intervention,
        "explicit-intervention-intent-v1", 1300);
    assert(responder.prepare_disposition(disposition).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(responder.bind_disposition_intent(
        disposition.disposition_id, intent.intent_id, 1400).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto bound = authority.bind_decision_intent(
        decision.decision_id, intent.intent_id, 1400);
    assert(bound.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(bound.decision);
    assert(bound.decision->state
        == LocalGooseDialogueResponderDecisionState::intent_bound);
    assert(bound.decision->intent_id
        == std::optional<std::string>{intent.intent_id});
    assert(bound.decision->bound_at_ms == std::optional<std::int64_t>{1400});

    constexpr const char* reason = "bound_intent_expired_before_dispatch";
    const auto expired = authority.terminalize_decision(
        decision.decision_id,
        LocalGooseDialogueResponderDecisionState::expired,
        reason, 9000);
    assert(expired.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(expired.decision);
    assert(expired.decision->state
        == LocalGooseDialogueResponderDecisionState::expired);
    assert(expired.decision->intent_id
        == std::optional<std::string>{intent.intent_id});
    assert(expired.decision->bound_at_ms == std::optional<std::int64_t>{1400});
    assert(expired.decision->terminal_reason == reason);
    assert(expired.decision->terminal_at_ms
        == std::optional<std::int64_t>{9000});
    assert(!expired.decision->completed_at_ms);

    const auto replay = authority.terminalize_decision(
        decision.decision_id,
        LocalGooseDialogueResponderDecisionState::expired,
        reason, 9000);
    assert(replay.result == LocalGooseDialogueResponderStoreResult::duplicate);

    LocalGooseDialogueResponderAuthorityStore reopened(file.path.string());
    const auto persisted = reopened.find_decision(decision.decision_id);
    assert(persisted);
    assert(persisted->state == LocalGooseDialogueResponderDecisionState::expired);
    assert(persisted->intent_id
        == std::optional<std::string>{intent.intent_id});
    assert(persisted->bound_at_ms == std::optional<std::int64_t>{1400});
    assert(persisted->terminal_reason == reason);
    assert(persisted->terminal_at_ms == std::optional<std::int64_t>{9000});
    assert(!persisted->completed_at_ms);

    return 0;
}
