#include "LocalGooseDialogueResponderRetryAuthority.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace gaudere_agent;

namespace {

std::string temp_path(const std::string& suffix)
{
    return "/tmp/gaudere-responder-retry-"
        + std::to_string(static_cast<unsigned long long>(getpid()))
        + "-" + suffix + ".db";
}

std::int64_t scalar(const std::string& path, const std::string& sql)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("cannot open retry test sqlite");
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
        throw std::runtime_error("retry scalar query returned no row");
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
        throw std::runtime_error("cannot open retry test sqlite");
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

std::string id(const std::string& prefix, const char c)
{
    return prefix + std::string(64, c);
}

LocalGooseDialogueResponderRetryLineage lineage(
    const std::string& request_id,
    const std::uint32_t generation,
    std::optional<std::string> previous,
    const std::int64_t created_at_ms,
    const std::string& event_id = id(
        "cognition.local-goose-dialogue.completion.v1:", '1'),
    const std::uint64_t sequence = 4,
    const std::string& decision_reason =
        "bound_intent_expired_before_dispatch")
{
    return make_local_goose_dialogue_responder_retry_lineage(
        request_id,
        "system-responder-v1",
        "main",
        sequence,
        event_id,
        std::string(64, '2'),
        3,
        id("cognition.local-goose-dialogue.v3:", '3'),
        "bertrand",
        generation,
        std::move(previous),
        id("cognition.local-goose-dialogue.responder-grant.v1:", '4'),
        "authority-lease-v1-5555555555555555555555555555555555555555555555555555555555555555",
        id("cognition.local-goose-dialogue.responder-decision.v1:", '6'),
        id("cognition.local-goose-dialogue.responder-intent.v1:", '7'),
        id("cognition.local-goose-dialogue.responder-disposition.v1:", '8'),
        "grant_expired",
        "lease_expired",
        decision_reason,
        "lease_or_intent_expired",
        "lease_or_intent_expired",
        created_at_ms);
}

} // namespace

int main()
{
    const auto path = temp_path("main");
    const auto future_path = temp_path("future");
    std::filesystem::remove(path);
    std::filesystem::remove(future_path);

    const auto first = lineage("retry-sequence4-generation1", 1, std::nullopt, 1000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(first));

    const auto first_replay = lineage(
        "retry-sequence4-generation1", 1, std::nullopt, 2000);
    assert(first_replay.lineage_id == first.lineage_id);

    LocalGooseDialogueResponderRetryStore store(path);
    assert(store.schema_version()
        == local_goose_dialogue_responder_retry_sidecar_schema);
    assert(scalar(path, "PRAGMA user_version;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name='local_goose_dialogue_responder_retry_lineage';") == 1);

    struct stat status {};
    assert(lstat(path.c_str(), &status) == 0);
    assert((status.st_mode & 0777) == 0600);

    const auto prepared = store.prepare_lineage(first);
    assert(prepared.result
        == LocalGooseDialogueResponderRetryStoreResult::accepted);
    assert(prepared.lineage && prepared.lineage->lineage_id == first.lineage_id);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage;") == 1);

    const auto duplicate = store.prepare_lineage(first_replay);
    assert(duplicate.result
        == LocalGooseDialogueResponderRetryStoreResult::duplicate);
    assert(duplicate.lineage && duplicate.lineage->created_at_ms == 1000);

    const auto loaded = store.find_lineage(first.lineage_id);
    assert(loaded && loaded->request_id == first.request_id);
    assert(store.find_by_request(first.request_id));
    assert(store.find_for_event_generation(
        first.consumer_id, first.event_id, 1));

    const auto same_request_changed_evidence = lineage(
        first.request_id, 1, std::nullopt, 1100,
        first.event_id, first.completion_sequence,
        "different_terminal_reason");
    assert(same_request_changed_evidence.lineage_id != first.lineage_id);
    assert(store.prepare_lineage(same_request_changed_evidence).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto same_event_generation = lineage(
        "retry-sequence4-generation1-conflict", 1, std::nullopt, 1200);
    assert(store.prepare_lineage(same_event_generation).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto same_sequence_generation = lineage(
        "retry-sequence4-generation1-sequence-conflict", 1, std::nullopt, 1300,
        id("cognition.local-goose-dialogue.completion.v1:", 'a'), 4);
    assert(store.prepare_lineage(same_sequence_generation).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto second = lineage(
        "retry-sequence4-generation2", 2, first.lineage_id, 2000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(second));
    const auto second_write = store.prepare_lineage(second);
    assert(second_write.result
        == LocalGooseDialogueResponderRetryStoreResult::accepted);
    assert(second_write.lineage
        && second_write.lineage->previous_retry_lineage_id
            == std::optional<std::string>{first.lineage_id});

    const auto skipped_predecessor = lineage(
        "retry-sequence4-generation3-wrong-parent", 3, first.lineage_id, 3000);
    assert(store.prepare_lineage(skipped_predecessor).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto third = lineage(
        "retry-sequence4-generation3", 3, second.lineage_id, 3000);
    assert(store.prepare_lineage(third).result
        == LocalGooseDialogueResponderRetryStoreResult::accepted);

    auto too_far = lineage(
        "retry-sequence4-generation9", 9, third.lineage_id, 4000);
    assert(!valid_local_goose_dialogue_responder_retry_lineage(too_far));
    assert(store.prepare_lineage(too_far).result
        == LocalGooseDialogueResponderRetryStoreResult::invalid);

    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage;") == 3);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE retry_generation=1 AND previous_retry_lineage_id IS NULL;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE retry_generation>1 AND previous_retry_lineage_id IS NOT NULL;") == 2);

    {
        LocalGooseDialogueResponderRetryStore fresh(future_path);
        assert(fresh.schema_version() == 1);
    }
    execute_sql(future_path, "PRAGMA user_version=2;");
    bool rejected_future = false;
    try {
        LocalGooseDialogueResponderRetryStore future(future_path);
    } catch (const std::exception&) {
        rejected_future = true;
    }
    assert(rejected_future);

    std::filesystem::remove(path);
    std::filesystem::remove(future_path);
    std::cout << "local_goose_dialogue_responder_retry_store_test: PASS\n";
    return 0;
}
