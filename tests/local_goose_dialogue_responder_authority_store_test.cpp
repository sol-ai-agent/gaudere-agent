#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace gaudere_agent;

namespace {

std::string temp_path(const std::string& suffix)
{
    return "/tmp/gaudere-responder-authority-"
        + std::to_string(static_cast<unsigned long long>(getpid()))
        + "-" + suffix + ".db";
}

std::int64_t scalar(const std::string& path, const std::string& sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("cannot open test sqlite");
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        const std::string error = sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error(error);
    }
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        sqlite3_close(db);
        throw std::runtime_error("test scalar query returned no row");
    }
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return value;
}

void execute_sql(const std::string& path, const std::string& sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("cannot open test sqlite");
    }
    char* error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        sqlite3_close(db);
        throw std::runtime_error(message);
    }
    sqlite3_close(db);
}

std::int64_t table_count(const std::string& path)
{
    return scalar(path,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%';");
}

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

LocalGooseDialogueResponderLease make_lease()
{
    LocalGooseDialogueResponderLease lease;
    lease.lease_id = "authority-proof-lease";
    lease.thread_alias = "main";
    lease.speaker_id = "sol";
    lease.allowed_message_kinds =
        local_goose_dialogue_responder_message_feedback
        | local_goose_dialogue_responder_message_intervention;
    lease.purpose = "Authority store proof";
    lease.max_system_turns = 2;
    lease.issued_at_ms = 2000;
    lease.expires_at_ms = 10000;
    lease.min_interval_ms = 100;
    lease.next_eligible_at_ms = lease.issued_at_ms;
    return lease;
}

} // namespace

int main()
{
    const auto path = temp_path("main");
    const auto future_path = temp_path("future");
    std::filesystem::remove(path);
    std::filesystem::remove(future_path);

    LocalGooseDialogueResponderStore responder(path);
    const auto lease = make_lease();
    assert(responder.create_lease(lease).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto historical_intent = make_local_goose_dialogue_responder_intent(
        lease.lease_id, "main", 2,
        completion_id('a'), std::string(64, 'b'), v3_id('c'),
        6, v3_id('c'), "sol", "feedback", "Historical intent.",
        2500, 7000);
    assert(responder.prepare_intent(historical_intent).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto historical_disposition =
        make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", 3,
            completion_id('d'), std::string(64, 'e'), 7, v3_id('f'),
            "system", "gaudere", 
            LocalGooseDialogueResponderDispositionKind::structural_skip_system,
            "observer-policy-v1", 2600);
    assert(responder.prepare_disposition(historical_disposition).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    assert(scalar(path, "PRAGMA user_version;") == 2);
    assert(table_count(path) == 3);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_intent;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition;") == 1);

    const auto grant = make_local_goose_dialogue_responder_grant(
        "grant-request-one", "local-typed-operator-v1", "main", "sol",
        lease.allowed_message_kinds, lease.purpose, 2, 9000, 100,
        1000, 12000);
    assert(valid_local_goose_dialogue_responder_grant(grant));

    {
        LocalGooseDialogueResponderAuthorityStore preserved(path);
        assert(preserved.schema_version() == 2);
        assert(!preserved.find_grant(grant.grant_id));
        const auto blocked = preserved.prepare_grant(grant);
        assert(blocked.result == LocalGooseDialogueResponderStoreResult::invalid);
    }
    assert(scalar(path, "PRAGMA user_version;") == 2);
    assert(table_count(path) == 3);

    LocalGooseDialogueResponderAuthorityStore authority(
        path,
        LocalGooseDialogueResponderAuthorityStoreOpenMode::allow_schema_upgrade);
    assert(authority.schema_version() == 3);
    assert(scalar(path, "PRAGMA user_version;") == 3);
    assert(table_count(path) == 5);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_lease;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_intent;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_disposition;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;") == 0);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;") == 0);

    const auto grant_write = authority.prepare_grant(grant);
    assert(grant_write.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(grant_write.grant && grant_write.grant->state
        == LocalGooseDialogueResponderGrantState::prepared);

    const auto grant_replay = make_local_goose_dialogue_responder_grant(
        "grant-request-one", "local-typed-operator-v1", "main", "sol",
        lease.allowed_message_kinds, lease.purpose, 2, 9000, 100,
        1100, 12000);
    assert(grant_replay.grant_id == grant.grant_id);
    const auto grant_duplicate = authority.prepare_grant(grant_replay);
    assert(grant_duplicate.result
        == LocalGooseDialogueResponderStoreResult::duplicate);
    assert(grant_duplicate.grant
        && grant_duplicate.grant->created_at_ms == grant.created_at_ms);

    const auto grant_conflict = make_local_goose_dialogue_responder_grant(
        "grant-request-one", "local-typed-operator-v1", "main", "sol",
        lease.allowed_message_kinds, lease.purpose, 1, 9000, 100,
        1200, 12000);
    assert(grant_conflict.grant_id != grant.grant_id);
    assert(authority.prepare_grant(grant_conflict).result
        == LocalGooseDialogueResponderStoreResult::conflict);

    assert(authority.activate_grant(
        grant.grant_id, "missing-lease", 3000).result
        == LocalGooseDialogueResponderStoreResult::conflict);
    const auto activated = authority.activate_grant(
        grant.grant_id, lease.lease_id, 3000);
    assert(activated.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(activated.grant && activated.grant->state
        == LocalGooseDialogueResponderGrantState::activated);
    assert(activated.grant->activated_lease_id
        == std::optional<std::string>{lease.lease_id});
    assert(authority.activate_grant(
        grant.grant_id, lease.lease_id, 3001).result
        == LocalGooseDialogueResponderStoreResult::duplicate);

    const auto decision = make_local_goose_dialogue_responder_decision(
        "decision-request-one", "local-typed-operator-v1",
        grant.grant_id, lease.lease_id, "system-responder-v1", 4,
        completion_id('1'), std::string(64, '2'), "main", 9, v3_id('3'),
        "human", "bertrand", 9, v3_id('3'), "sol", "feedback",
        "Decision message.", 5000, 3100);
    assert(valid_local_goose_dialogue_responder_decision(decision));
    const auto decision_write = authority.prepare_decision(decision);
    assert(decision_write.result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto decision_replay = make_local_goose_dialogue_responder_decision(
        "decision-request-one", "local-typed-operator-v1",
        grant.grant_id, lease.lease_id, "system-responder-v1", 4,
        completion_id('1'), std::string(64, '2'), "main", 9, v3_id('3'),
        "human", "bertrand", 9, v3_id('3'), "sol", "feedback",
        "Decision message.", 5000, 3150);
    assert(decision_replay.decision_id == decision.decision_id);
    assert(authority.prepare_decision(decision_replay).result
        == LocalGooseDialogueResponderStoreResult::duplicate);

    const auto decision_conflict = make_local_goose_dialogue_responder_decision(
        "decision-request-one", "local-typed-operator-v1",
        grant.grant_id, lease.lease_id, "system-responder-v1", 4,
        completion_id('1'), std::string(64, '2'), "main", 9, v3_id('3'),
        "human", "bertrand", 9, v3_id('3'), "sol", "feedback",
        "Different message.", 5000, 3150);
    assert(decision_conflict.decision_id != decision.decision_id);
    assert(authority.prepare_decision(decision_conflict).result
        == LocalGooseDialogueResponderStoreResult::conflict);

    const auto exact_intent = make_local_goose_dialogue_responder_intent(
        lease.lease_id, "main", 4,
        completion_id('1'), std::string(64, '2'), v3_id('3'),
        9, v3_id('3'), "sol", "feedback", "Decision message.",
        3200, 7000);
    assert(responder.prepare_intent(exact_intent).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    assert(authority.bind_decision_intent(
        decision.decision_id, historical_intent.intent_id, 3300).result
        == LocalGooseDialogueResponderStoreResult::conflict);

    const auto intervention_disposition =
        make_local_goose_dialogue_responder_disposition(
            "system-responder-v1", "main", 4,
            completion_id('1'), std::string(64, '2'), 9, v3_id('3'),
            "human", "bertrand",
            LocalGooseDialogueResponderDispositionKind::intervention,
            "explicit-intervention-intent-v1", 3200);
    assert(responder.prepare_disposition(intervention_disposition).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(responder.bind_disposition_intent(
        intervention_disposition.disposition_id,
        exact_intent.intent_id, 3300).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto bound = authority.bind_decision_intent(
        decision.decision_id, exact_intent.intent_id, 3300);
    assert(bound.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(bound.decision && bound.decision->state
        == LocalGooseDialogueResponderDecisionState::intent_bound);
    assert(authority.bind_decision_intent(
        decision.decision_id, exact_intent.intent_id, 3301).result
        == LocalGooseDialogueResponderStoreResult::duplicate);

    assert(responder.commit_submission(
        exact_intent.intent_id, v3_id('4'), 10, 3400).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(responder.complete_intent(exact_intent.intent_id, 3500).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(responder.complete_disposition(
        intervention_disposition.disposition_id, 3500).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    const auto completed = authority.complete_decision(
        decision.decision_id, 3600);
    assert(completed.result == LocalGooseDialogueResponderStoreResult::accepted);
    assert(completed.decision && completed.decision->state
        == LocalGooseDialogueResponderDecisionState::completed);
    assert(authority.complete_decision(
        decision.decision_id, 3600).result
        == LocalGooseDialogueResponderStoreResult::duplicate);

    const auto decision_two = make_local_goose_dialogue_responder_decision(
        "decision-request-two", "local-typed-operator-v1",
        grant.grant_id, lease.lease_id, "system-responder-v1", 6,
        completion_id('5'), std::string(64, '6'), "main", 11, v3_id('7'),
        "human", "bertrand", 11, v3_id('7'), "sol", "feedback",
        "Second explicit decision.", 1000, 3450);
    assert(authority.prepare_decision(decision_two).result
        == LocalGooseDialogueResponderStoreResult::accepted);
    assert(authority.terminalize_decision(
        decision_two.decision_id,
        LocalGooseDialogueResponderDecisionState::conflict,
        "head_changed", 3460).result
        == LocalGooseDialogueResponderStoreResult::accepted);

    {
        LocalGooseDialogueResponderAuthorityStore reopened(path);
        assert(reopened.schema_version() == 3);
        const auto persisted_grant = reopened.find_grant(grant.grant_id);
        assert(persisted_grant && persisted_grant->state
            == LocalGooseDialogueResponderGrantState::activated);
        const auto persisted_decision = reopened.find_decision(decision.decision_id);
        assert(persisted_decision && persisted_decision->state
            == LocalGooseDialogueResponderDecisionState::completed);
        const auto conflicts = reopened.decisions_in_state(
            LocalGooseDialogueResponderDecisionState::conflict);
        assert(conflicts.size() == 1);
    }

    {
        LocalGooseDialogueResponderStore future(future_path);
        assert(scalar(future_path, "PRAGMA user_version;") == 2);
    }
    execute_sql(future_path, "PRAGMA user_version=4;");
    bool rejected_future = false;
    try {
        LocalGooseDialogueResponderAuthorityStore future(future_path);
    } catch (const std::exception&) {
        rejected_future = true;
    }
    assert(rejected_future);

    std::filesystem::remove(path);
    std::filesystem::remove(future_path);
    std::cout << "local_goose_dialogue_responder_authority_store_test: PASS\n";
    return 0;
}
