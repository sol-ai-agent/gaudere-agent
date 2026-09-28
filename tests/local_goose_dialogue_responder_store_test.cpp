#include "LocalGooseDialogueResponderStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <string>
#include <sys/stat.h>

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

} // namespace

int main()
{
    using namespace gaudere_agent;

    TemporarySidecar sidecar;
    {
        LocalGooseDialogueResponderStore store(sidecar.path.string());

        struct stat status {};
        assert(::stat(sidecar.path.c_str(), &status) == 0);
        assert(S_ISREG(status.st_mode));
        assert((status.st_mode & 0777) == 0600);

        sqlite3* database = nullptr;
        assert(sqlite3_open_v2(
            sidecar.path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
            == SQLITE_OK);
        sqlite3_stmt* statement = nullptr;
        assert(sqlite3_prepare_v2(
            database, "PRAGMA user_version", -1, &statement, nullptr)
            == SQLITE_OK);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        assert(sqlite3_column_int(statement, 0) == 1);
        sqlite3_finalize(statement);
        sqlite3_close(database);

        auto lease1 = lease("lease-001");
        assert(valid_local_goose_dialogue_responder_lease(lease1));
        assert(store.create_lease(lease1).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.create_lease(lease1).result
            == LocalGooseDialogueResponderStoreResult::duplicate);
        assert(store.find_active_lease("main"));

        auto same_id_conflict = lease1;
        same_id_conflict.purpose = "Different";
        assert(store.create_lease(same_id_conflict).result
            == LocalGooseDialogueResponderStoreResult::conflict);
        auto parallel = lease("lease-002");
        assert(store.create_lease(parallel).result
            == LocalGooseDialogueResponderStoreResult::conflict);

        auto bad_turns = lease("bad-turns", 9);
        assert(!valid_local_goose_dialogue_responder_lease(bad_turns));
        assert(store.create_lease(bad_turns).result
            == LocalGooseDialogueResponderStoreResult::invalid);
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

        assert(store.prepare_intent(intent1).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.prepare_intent(intent1).result
            == LocalGooseDialogueResponderStoreResult::duplicate);
        assert(store.prepare_intent(changed).result
            == LocalGooseDialogueResponderStoreResult::conflict);

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
        assert(store.prepare_intent(disallowed).result
            == LocalGooseDialogueResponderStoreResult::conflict);

        const auto submitted1 = v3_id('e');
        const auto committed1 = store.commit_submission(
            intent1.intent_id, submitted1, 2, 1100);
        assert(committed1.result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(committed1.intent);
        assert(committed1.intent->state
            == LocalGooseDialogueResponderIntentState::submitted);
        assert(store.commit_submission(
            intent1.intent_id, submitted1, 2, 1100).result
            == LocalGooseDialogueResponderStoreResult::duplicate);

        const auto after1 = store.find_lease(lease1.lease_id);
        assert(after1);
        assert(after1->turns_committed == 1);
        assert(after1->next_eligible_at_ms == 1200);
        assert(after1->state
            == LocalGooseDialogueResponderLeaseState::active);

        assert(store.complete_intent(intent1.intent_id, 1300).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.complete_intent(intent1.intent_id, 1300).result
            == LocalGooseDialogueResponderStoreResult::duplicate);

        auto too_early = intent(
            lease1, 2, 'c', 'd', 2, "Too early", 1199);
        assert(store.prepare_intent(too_early).result
            == LocalGooseDialogueResponderStoreResult::conflict);

        auto intent2 = intent(
            lease1, 2, 'c', 'd', 2, "Second feedback", 1200);
        assert(store.prepare_intent(intent2).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.commit_submission(
            intent2.intent_id, v3_id('f'), 3, 1250).result
            == LocalGooseDialogueResponderStoreResult::accepted);

        const auto exhausted = store.find_lease(lease1.lease_id);
        assert(exhausted);
        assert(exhausted->turns_committed == 2);
        assert(exhausted->state
            == LocalGooseDialogueResponderLeaseState::exhausted);
        assert(!store.find_active_lease("main"));

        auto after_exhaustion = intent(
            lease1, 3, 'e', 'f', 3, "Third", 1400);
        assert(store.prepare_intent(after_exhaustion).result
            == LocalGooseDialogueResponderStoreResult::conflict);

        auto lease2 = lease("lease-002", 1, 2000, 0);
        assert(store.create_lease(lease2).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        auto prepared_terminal = intent(
            lease2, 3, 'e', 'f', 3, "Will conflict", 2000);
        assert(store.prepare_intent(prepared_terminal).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.terminalize_intent(
            prepared_terminal.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "head_changed").result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.terminalize_intent(
            prepared_terminal.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "head_changed").result
            == LocalGooseDialogueResponderStoreResult::duplicate);

        assert(store.close_lease(
            lease2.lease_id,
            LocalGooseDialogueResponderLeaseState::revoked,
            "operator_stop",
            2100).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.close_lease(
            lease2.lease_id,
            LocalGooseDialogueResponderLeaseState::revoked,
            "operator_stop",
            2100).result
            == LocalGooseDialogueResponderStoreResult::duplicate);

        auto lease3 = lease("lease-003", 1, 3000, 0);
        assert(store.create_lease(lease3).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        auto intent3 = intent(
            lease3, 4, '1', '2', 4, "Submit then review", 3000);
        assert(store.prepare_intent(intent3).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.commit_submission(
            intent3.intent_id, v3_id('3'), 5, 3010).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(store.terminalize_intent(
            intent3.intent_id,
            LocalGooseDialogueResponderIntentState::conflict,
            "not_allowed_after_submit").result
            == LocalGooseDialogueResponderStoreResult::conflict);
        assert(store.terminalize_intent(
            intent3.intent_id,
            LocalGooseDialogueResponderIntentState::manual_review,
            "ambiguous_ack_state").result
            == LocalGooseDialogueResponderStoreResult::accepted);

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
    }

    {
        LocalGooseDialogueResponderStore reopened(sidecar.path.string());
        const auto lease1 = reopened.find_lease("lease-001");
        assert(lease1);
        assert(lease1->state
            == LocalGooseDialogueResponderLeaseState::exhausted);
        assert(lease1->turns_committed == 2);
        const auto completed = reopened.intents_in_state(
            LocalGooseDialogueResponderIntentState::completed, 16);
        assert(completed.size() == 1);
        assert(reopened.find_intent(completed.front().intent_id));
    }

    {
        TemporarySidecar future;
        sqlite3* database = nullptr;
        assert(sqlite3_open(future.path.c_str(), &database) == SQLITE_OK);
        assert(sqlite3_exec(
            database, "PRAGMA user_version=2;", nullptr, nullptr, nullptr)
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
