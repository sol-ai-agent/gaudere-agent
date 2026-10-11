#include "LocalGooseDialogueResponderStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace {

using namespace gaudere_agent;

std::string sha_id(const std::string& prefix, const char digit)
{
    return prefix + std::string(64, digit);
}

std::string event_id(const char digit)
{
    return sha_id(
        "cognition.local-goose-dialogue.completion.v1:", digit);
}

std::string v3_id(const char digit)
{
    return sha_id("cognition.local-goose-dialogue.v3:", digit);
}

struct TemporarySidecar {
    TemporarySidecar()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-dialogue-responder-store-test-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch().count())
               + ".db");
    }

    ~TemporarySidecar()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
    }

    std::filesystem::path path;
};

LocalGooseDialogueResponderLease lease(
    std::string id,
    const std::uint64_t max_turns = 2,
    const std::int64_t issued = 1000,
    const std::int64_t interval = 100)
{
    LocalGooseDialogueResponderLease out;
    out.lease_id = std::move(id);
    out.thread_alias = "main";
    out.speaker_id = "sol";
    out.allowed_message_kinds =
        local_goose_dialogue_responder_message_feedback
        | local_goose_dialogue_responder_message_intervention;
    out.purpose = "Bounded test intervention";
    out.max_system_turns = max_turns;
    out.issued_at_ms = issued;
    out.expires_at_ms = issued + 10000;
    out.min_interval_ms = interval;
    out.next_eligible_at_ms = issued;
    return out;
}

LocalGooseDialogueResponderIntent intent(
    const LocalGooseDialogueResponderLease& responder_lease,
    const std::uint64_t sequence,
    const char event_digit,
    const char task_digit,
    const std::uint64_t revision,
    std::string message,
    const std::int64_t created)
{
    return make_local_goose_dialogue_responder_intent(
        responder_lease.lease_id,
        responder_lease.thread_alias,
        sequence,
        event_id(event_digit),
        std::string(64, event_digit),
        v3_id(task_digit),
        revision,
        v3_id(task_digit),
        responder_lease.speaker_id,
        "feedback",
        message,
        created,
        created + 500);
}

LocalGooseDialogueResponderDisposition disposition(
    const std::string& consumer,
    const std::uint64_t sequence,
    const char event_digit,
    const char task_digit,
    const std::uint64_t revision,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const LocalGooseDialogueResponderDispositionKind kind,
    const std::string& authority,
    const std::int64_t created)
{
    return make_local_goose_dialogue_responder_disposition(
        consumer,
        "main",
        sequence,
        event_id(event_digit),
        std::string(64, event_digit),
        revision,
        v3_id(task_digit),
        speaker_kind,
        speaker_id,
        kind,
        authority,
        created);
}

int pragma_user_version(const std::filesystem::path& path)
{
    sqlite3* database = nullptr;
    assert(sqlite3_open_v2(
        path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(
        database, "PRAGMA user_version", -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

int user_table_count(const std::filesystem::path& path)
{
    sqlite3* database = nullptr;
    assert(sqlite3_open_v2(
        path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    assert(sqlite3_prepare_v2(
        database,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%'",
        -1,
        &statement,
        nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

} // namespace

int main()
{
    using namespace gaudere_agent;
    using StoreResult = LocalGooseDialogueResponderStoreResult;
    using StoreOpenMode = LocalGooseDialogueResponderStoreOpenMode;
    using DispositionKind = LocalGooseDialogueResponderDispositionKind;
    using DispositionState = LocalGooseDialogueResponderDispositionState;

    TemporarySidecar sidecar;
    std::string completed_disposition_id;
    std::string acknowledged_disposition_id;
    {
        LocalGooseDialogueResponderStore store(sidecar.path.string());

        struct stat status {};
        assert(::stat(sidecar.path.c_str(), &status) == 0);
        assert(S_ISREG(status.st_mode));
        assert((status.st_mode & 0777) == 0600);
        assert(pragma_user_version(sidecar.path) == 2);
        assert(user_table_count(sidecar.path) == 3);

        auto lease1 = lease("lease-001");
        assert(valid_local_goose_dialogue_responder_lease(lease1));
        assert(store.create_lease(lease1).result == StoreResult::accepted);
        assert(store.create_lease(lease1).result == StoreResult::duplicate);
        assert(store.find_active_lease("main"));

        auto same_id_conflict = lease1;
        same_id_conflict.purpose = "Different";
        assert(store.create_lease(same_id_conflict).result == StoreResult::conflict);
        auto parallel = lease("lease-002");
        assert(store.create_lease(parallel).result == StoreResult::conflict);

        auto bad_turns = lease("bad-turns", 9);
        assert(!valid_local_goose_dialogue_responder_lease(bad_turns));
        assert(store.create_lease(bad_turns).result == StoreResult::invalid);
        auto bad_kind = lease("bad-kind");
        bad_kind.speaker_kind = "human";
        assert(!valid_local_goose_dialogue_responder_lease(bad_kind));
        auto bad_mask = lease("bad-mask");
        bad_mask.allowed_message_kinds = 1u << 20;
        assert(!valid_local_goose_dialogue_responder_lease(bad_mask));
        auto bad_duration = lease("bad-duration");
        bad_duration.expires_at_ms =
            bad_duration.issued_at_ms + 24LL * 60 * 60 * 1000 + 1;
        assert(!valid_local_goose_dialogue_responder_lease(bad_duration));

        auto intent1 = intent(
            lease1, 1, 'a', 'b', 1, "First feedback", 1000);
        assert(valid_local_goose_dialogue_responder_intent(intent1));
        const auto identical = intent(
            lease1, 1, 'a', 'b', 1, "First feedback", 1000);
        assert(intent1.intent_id == identical.intent_id);
        assert(intent1.request_id == identical.request_id);
        const auto changed = intent(
            lease1, 1, 'a', 'b', 1, "Changed feedback", 1000);
        assert(intent1.intent_id != changed.intent_id);

        assert(store.prepare_intent(intent1).result == StoreResult::accepted);
        assert(store.prepare_intent(intent1).result == StoreResult::duplicate);
        assert(store.prepare_intent(changed).result == StoreResult::conflict);

        auto disallowed = make_local_goose_dialogue_responder_intent(
            lease1.lease_id,
            "main",
            2,
            event_id('c'),
            std::string(64, 'c'),
            v3_id('d'),
            1,
            v3_id('d'),
            "sol",
            "observation",
            "Not admitted",
            1000,
            1500);
        assert(valid_local_goose_dialogue_responder_intent(disallowed));
        assert(store.prepare_intent(disallowed).result == StoreResult::conflict);

        const auto submitted1 = v3_id('e');
        const auto committed1 = store.commit_submission(
            intent1.intent_id, submitted1, 2, 1100);
        assert(committed1.result == StoreResult::accepted);
        assert(committed1.intent);
        assert(committed1.intent->state
            == LocalGooseDialogueResponderIntentState::submitted);
        assert(store.commit_submission(
            intent1.intent_id, submitted1, 2, 1100).result
            == StoreResult::duplicate);

        const auto after1 = store.find_lease(lease1.lease_id);
        assert(after1);
        assert(after1->turns_committed == 1);
        assert(after1->next_eligible_at_ms == 1200);
        assert(after1->state == LocalGooseDialogueResponderLeaseState::active);

        assert(store.complete_intent(intent1.intent_id, 1300).result
            == StoreResult::accepted);
        assert(store.complete_intent(intent1.intent_id, 1300).result
            == StoreResult::duplicate);

        auto too_early = intent(
            lease1, 2, 'c', 'd', 2, "Too early", 1199);
        assert(store.prepare_intent(too_early).result == StoreResult::conflict);

        auto intent2 = intent(
            lease1, 2, 'c', 'd', 2, "Second feedback", 1200);
        assert(store.prepare_intent(intent2).result == StoreResult::accepted);
        assert(store.commit_submission(
            intent2.intent_id, v3_id('f'), 3, 1250).result
            == StoreResult::accepted);

        const auto exhausted = store.find_lease(lease1.lease_id);
        assert(exhausted);
        assert(exhausted->turns_committed == 2);
        assert(exhausted->state
            == LocalGooseDialogueResponderLeaseState::exhausted);
        assert(!store.find_active_lease("main"));

        auto after_exhaustion = intent(
            lease1, 3, 'e', 'f', 3, "Third", 1400);
        assert(store.prepare_intent(after_exhaustion).result
            == StoreResult::conflict);

        auto lease2 = lease("lease-002", 1, 2000, 0);
        assert(store.create_lease(lease2).result == StoreResult::accepted);
        auto prepared_terminal = intent(
            lease2, 3, 'e', 'f', 3, "Will conflict", 2000);
        assert(store.prepare_intent(prepared_terminal).result
            == StoreResult::accepted);
        assert(store.terminalize_intent(
            prepared_terminal.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "head_changed").result == StoreResult::accepted);
        assert(store.terminalize_intent(
            prepared_terminal.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "head_changed").result == StoreResult::duplicate);

        assert(store.close_lease(
            lease2.lease_id,
            LocalGooseDialogueResponderLeaseState::revoked,
            "operator_stop",
            2100).result == StoreResult::accepted);
        assert(store.close_lease(
            lease2.lease_id,
            LocalGooseDialogueResponderLeaseState::revoked,
            "operator_stop",
            2100).result == StoreResult::duplicate);

        auto lease3 = lease("lease-003", 1, 3000, 0);
        assert(store.create_lease(lease3).result == StoreResult::accepted);
        auto intent3 = intent(
            lease3, 4, '1', '2', 4, "Submit then review", 3000);
        assert(store.prepare_intent(intent3).result == StoreResult::accepted);
        assert(store.commit_submission(
            intent3.intent_id, v3_id('3'), 5, 3010).result
            == StoreResult::accepted);
        assert(store.terminalize_intent(
            intent3.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "not_allowed_after_submit").result == StoreResult::conflict);
        assert(store.terminalize_intent(
            intent3.intent_id,
            LocalGooseDialogueResponderIntentState::manual_review,
            "ambiguous_ack_state").result == StoreResult::accepted);

        const auto conflicts = store.intents_in_state(
            LocalGooseDialogueResponderIntentState::conflict, 16);
        assert(conflicts.size() == 1);
        assert(conflicts.front().intent_id == prepared_terminal.intent_id);
        const auto reviews = store.intents_in_state(
            LocalGooseDialogueResponderIntentState::manual_review, 16);
        assert(reviews.size() == 1);
        assert(reviews.front().intent_id == intent3.intent_id);
        assert(store.intents_in_state(
            LocalGooseDialogueResponderIntentState::prepared, 0).empty());

        // Structural system-event disposition: deterministic identity, exact
        // immutable duplicate semantics, and no intent/lease requirement.
        auto system_skip = disposition(
            "system-responder-v1",
            10,
            '4',
            '5',
            9,
            "system",
            "sol",
            DispositionKind::structural_skip_system,
            "observer-policy-v1",
            4000);
        assert(valid_local_goose_dialogue_responder_disposition(system_skip));
        const auto system_skip_same = disposition(
            "system-responder-v1",
            10,
            '4',
            '5',
            9,
            "system",
            "sol",
            DispositionKind::structural_skip_system,
            "observer-policy-v1",
            4000);
        assert(system_skip.disposition_id == system_skip_same.disposition_id);
        assert(store.prepare_disposition(system_skip).result
            == StoreResult::accepted);
        assert(store.prepare_disposition(system_skip_same).result
            == StoreResult::duplicate);

        auto different_authority = system_skip;
        different_authority.authority_source = "operator-v1";
        assert(valid_local_goose_dialogue_responder_disposition(
            different_authority));
        assert(different_authority.disposition_id == system_skip.disposition_id);
        assert(store.prepare_disposition(different_authority).result
            == StoreResult::conflict);

        // The same consumer/event has exactly one cause even when a different
        // kind would independently be structurally valid.
        auto same_event_other_kind = disposition(
            "system-responder-v1",
            10,
            '4',
            '5',
            9,
            "system",
            "sol",
            DispositionKind::structural_skip_stale,
            "observer-policy-v1",
            4000);
        assert(valid_local_goose_dialogue_responder_disposition(
            same_event_other_kind));
        assert(same_event_other_kind.disposition_id
            != system_skip.disposition_id);
        assert(store.prepare_disposition(same_event_other_kind).result
            == StoreResult::conflict);

        // Sequence uniqueness is independently enforced for one consumer.
        auto same_sequence_other_event = disposition(
            "system-responder-v1",
            10,
            '5',
            '6',
            10,
            "system",
            "sol",
            DispositionKind::structural_skip_stale,
            "observer-policy-v1",
            4000);
        assert(store.prepare_disposition(same_sequence_other_event).result
            == StoreResult::conflict);

        // A different consumer may independently record its own disposition.
        auto other_consumer = disposition(
            "audit-responder-v1",
            10,
            '4',
            '5',
            9,
            "system",
            "sol",
            DispositionKind::structural_skip_system,
            "observer-policy-v1",
            4000);
        assert(store.prepare_disposition(other_consumer).result
            == StoreResult::accepted);

        auto invalid_system_skip = disposition(
            "system-responder-v1",
            11,
            '5',
            '6',
            10,
            "human",
            "bertrand",
            DispositionKind::structural_skip_system,
            "observer-policy-v1",
            4010);
        assert(!valid_local_goose_dialogue_responder_disposition(
            invalid_system_skip));
        auto invalid_decline = disposition(
            "system-responder-v1",
            11,
            '5',
            '6',
            10,
            "system",
            "sol",
            DispositionKind::explicit_decline,
            "operator-v1",
            4010);
        assert(!valid_local_goose_dialogue_responder_disposition(
            invalid_decline));

        assert(store.bind_disposition_intent(
            system_skip.disposition_id, intent1.intent_id, 4100).result
            == StoreResult::conflict);
        assert(store.acknowledge_disposition(
            system_skip.disposition_id, 4100).result == StoreResult::accepted);
        assert(store.acknowledge_disposition(
            system_skip.disposition_id, 4100).result == StoreResult::duplicate);
        assert(store.acknowledge_disposition(
            system_skip.disposition_id, 4101).result == StoreResult::conflict);
        acknowledged_disposition_id = system_skip.disposition_id;

        const auto acknowledged = store.find_disposition_for_event(
            "system-responder-v1", system_skip.event_id);
        assert(acknowledged);
        assert(acknowledged->state == DispositionState::acknowledged);
        assert(!acknowledged->intent_id);

        // Current human event: explicit decline is structurally valid and can
        // be durably reconciled without any responder intent.
        auto decline = disposition(
            "system-responder-v1",
            11,
            '5',
            '6',
            10,
            "human",
            "bertrand",
            DispositionKind::explicit_decline,
            "operator-v1",
            4200);
        assert(valid_local_goose_dialogue_responder_disposition(decline));
        assert(store.prepare_disposition(decline).result == StoreResult::accepted);
        assert(store.acknowledge_disposition(decline.disposition_id, 4210).result
            == StoreResult::accepted);

        // Intervention disposition: exact prepared intent evidence is required
        // before binding, and the disposition cannot complete until the bound
        // intent itself is durably completed.
        auto lease4 = lease("lease-004", 1, 5000, 0);
        assert(store.create_lease(lease4).result == StoreResult::accepted);
        auto intervention_intent = intent(
            lease4, 12, '6', '7', 11, "Bound intervention", 5000);
        assert(store.prepare_intent(intervention_intent).result
            == StoreResult::accepted);
        auto intervention = disposition(
            "system-responder-v1",
            12,
            '6',
            '7',
            11,
            "human",
            "bertrand",
            DispositionKind::intervention,
            "operator-v1",
            5000);
        assert(store.prepare_disposition(intervention).result
            == StoreResult::accepted);
        assert(store.acknowledge_disposition(
            intervention.disposition_id, 5001).result == StoreResult::conflict);

        auto mismatched_intent = intent(
            lease4, 13, '7', '8', 12, "Wrong event", 5000);
        // The second intent cannot be prepared under the same one-turn lease if
        // the matching intervention is still pending, but the binding surface
        // also independently rejects any absent/mismatched intent id.
        assert(store.bind_disposition_intent(
            intervention.disposition_id,
            mismatched_intent.intent_id,
            5010).result == StoreResult::conflict);
        assert(store.bind_disposition_intent(
            intervention.disposition_id,
            intervention_intent.intent_id,
            5010).result == StoreResult::accepted);
        assert(store.bind_disposition_intent(
            intervention.disposition_id,
            intervention_intent.intent_id,
            5010).result == StoreResult::duplicate);
        assert(store.complete_disposition(
            intervention.disposition_id, 5020).result == StoreResult::conflict);

        assert(store.commit_submission(
            intervention_intent.intent_id, v3_id('8'), 12, 5020).result
            == StoreResult::accepted);
        assert(store.complete_intent(
            intervention_intent.intent_id, 5030).result == StoreResult::accepted);
        assert(store.complete_disposition(
            intervention.disposition_id, 5029).result == StoreResult::conflict);
        assert(store.complete_disposition(
            intervention.disposition_id, 5040).result == StoreResult::accepted);
        assert(store.complete_disposition(
            intervention.disposition_id, 5040).result == StoreResult::duplicate);
        completed_disposition_id = intervention.disposition_id;

        const auto completed_dispositions = store.dispositions_in_state(
            DispositionState::completed, 16);
        assert(completed_dispositions.size() == 1);
        assert(completed_dispositions.front().disposition_id
            == intervention.disposition_id);
        assert(store.dispositions_in_state(
            DispositionState::prepared, 0).empty());

        // Manual-review transition is idempotent and works both before binding
        // and after an intervention was bound.
        auto review = disposition(
            "system-responder-v1",
            13,
            '9',
            'a',
            12,
            "system",
            "sol",
            DispositionKind::structural_skip_stale,
            "observer-policy-v1",
            6000);
        assert(store.prepare_disposition(review).result == StoreResult::accepted);
        assert(store.terminalize_disposition(
            review.disposition_id, "ambiguous_cursor", 6010).result
            == StoreResult::accepted);
        assert(store.terminalize_disposition(
            review.disposition_id, "ambiguous_cursor", 6010).result
            == StoreResult::duplicate);
        assert(store.terminalize_disposition(
            review.disposition_id, "different_reason", 6010).result
            == StoreResult::conflict);

        const auto disposition_reviews = store.dispositions_in_state(
            DispositionState::manual_review, 16);
        assert(disposition_reviews.size() == 1);
        assert(disposition_reviews.front().disposition_id
            == review.disposition_id);
    }

    {
        LocalGooseDialogueResponderStore reopened(sidecar.path.string());
        assert(pragma_user_version(sidecar.path) == 2);
        assert(user_table_count(sidecar.path) == 3);
        const auto lease1 = reopened.find_lease("lease-001");
        assert(lease1);
        assert(lease1->state
            == LocalGooseDialogueResponderLeaseState::exhausted);
        assert(lease1->turns_committed == 2);
        const auto completed = reopened.intents_in_state(
            LocalGooseDialogueResponderIntentState::completed, 16);
        assert(completed.size() == 2);
        assert(reopened.find_disposition(completed_disposition_id));
        assert(reopened.find_disposition(acknowledged_disposition_id));
        assert(reopened.find_disposition(completed_disposition_id)->state
            == DispositionState::completed);
        assert(reopened.find_disposition(acknowledged_disposition_id)->state
            == DispositionState::acknowledged);
    }

    // Existing v1 sidecars are preserved on ordinary open. Migration to v2 is
    // separately explicit, preserves lease/intent evidence, creates only the
    // empty disposition table, and synthesizes no historical disposition cause.
    {
        TemporarySidecar migration;
        std::string legacy_intent_id;
        {
            LocalGooseDialogueResponderStore initial(migration.path.string());
            auto legacy_lease = lease("legacy-lease", 1, 7000, 0);
            assert(initial.create_lease(legacy_lease).result
                == StoreResult::accepted);
            auto legacy_intent = intent(
                legacy_lease, 21, 'b', 'c', 20, "Legacy pending", 7000);
            legacy_intent_id = legacy_intent.intent_id;
            assert(initial.prepare_intent(legacy_intent).result
                == StoreResult::accepted);
        }

        sqlite3* database = nullptr;
        assert(sqlite3_open(migration.path.c_str(), &database) == SQLITE_OK);
        assert(sqlite3_exec(database, "PRAGMA foreign_keys=OFF;", nullptr, nullptr, nullptr)
            == SQLITE_OK);
        assert(sqlite3_exec(
            database,
            "DROP TABLE local_goose_dialogue_responder_disposition;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK);
        assert(sqlite3_exec(database, "PRAGMA user_version=1;", nullptr, nullptr, nullptr)
            == SQLITE_OK);
        sqlite3_close(database);
        assert(::chmod(migration.path.c_str(), 0600) == 0);
        assert(pragma_user_version(migration.path) == 1);
        assert(user_table_count(migration.path) == 2);

        {
            LocalGooseDialogueResponderStore preserved(migration.path.string());
            assert(pragma_user_version(migration.path) == 1);
            assert(user_table_count(migration.path) == 2);
            const auto legacy_lease = preserved.find_lease("legacy-lease");
            assert(legacy_lease);
            assert(legacy_lease->state
                == LocalGooseDialogueResponderLeaseState::active);
            const auto legacy_intent = preserved.find_intent(legacy_intent_id);
            assert(legacy_intent);
            assert(legacy_intent->state
                == LocalGooseDialogueResponderIntentState::prepared);
        }

        assert(pragma_user_version(migration.path) == 1);
        assert(user_table_count(migration.path) == 2);

        LocalGooseDialogueResponderStore migrated(
            migration.path.string(), StoreOpenMode::allow_schema_upgrade);
        assert(pragma_user_version(migration.path) == 2);
        assert(user_table_count(migration.path) == 3);
        const auto legacy_lease = migrated.find_lease("legacy-lease");
        assert(legacy_lease);
        assert(legacy_lease->state == LocalGooseDialogueResponderLeaseState::active);
        const auto legacy_intent = migrated.find_intent(legacy_intent_id);
        assert(legacy_intent);
        assert(legacy_intent->state
            == LocalGooseDialogueResponderIntentState::prepared);
        assert(migrated.dispositions_in_state(
            DispositionState::prepared, 64).empty());
        assert(migrated.dispositions_in_state(
            DispositionState::acknowledged, 64).empty());
        assert(migrated.dispositions_in_state(
            DispositionState::intervention_bound, 64).empty());
        assert(migrated.dispositions_in_state(
            DispositionState::completed, 64).empty());
        assert(migrated.dispositions_in_state(
            DispositionState::manual_review, 64).empty());
    }

    {
        TemporarySidecar future;
        sqlite3* database = nullptr;
        assert(sqlite3_open(future.path.c_str(), &database) == SQLITE_OK);
        assert(sqlite3_exec(
            database, "PRAGMA user_version=3;", nullptr, nullptr, nullptr)
            == SQLITE_OK);
        sqlite3_close(database);
        assert(::chmod(future.path.c_str(), 0600) == 0);
        bool rejected = false;
        try {
            LocalGooseDialogueResponderStore unsupported(future.path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        TemporarySidecar malformed_v2;
        sqlite3* database = nullptr;
        assert(sqlite3_open(malformed_v2.path.c_str(), &database) == SQLITE_OK);
        assert(sqlite3_exec(
            database,
            "CREATE TABLE wrong(value INTEGER); PRAGMA user_version=2;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK);
        sqlite3_close(database);
        assert(::chmod(malformed_v2.path.c_str(), 0600) == 0);
        bool rejected = false;
        try {
            LocalGooseDialogueResponderStore malformed(
                malformed_v2.path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    {
        TemporarySidecar unversioned;
        sqlite3* database = nullptr;
        assert(sqlite3_open(unversioned.path.c_str(), &database) == SQLITE_OK);
        assert(sqlite3_exec(
            database, "CREATE TABLE unexpected(value INTEGER);",
            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(database);
        assert(::chmod(unversioned.path.c_str(), 0600) == 0);
        bool rejected = false;
        try {
            LocalGooseDialogueResponderStore dirty(unversioned.path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    std::cout << "local_goose_dialogue_responder_store_test: PASS\n";
    return 0;
}
