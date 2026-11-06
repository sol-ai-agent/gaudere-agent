#include "LocalGooseDialogueResponderRetryAuthority.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string byte_snapshot(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
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
        id("cognition.local-goose-dialogue.responder-grant.v1:", '4'),
        "authority-lease-v1-5555555555555555555555555555555555555555555555555555555555555555",
        id("cognition.local-goose-dialogue.responder-decision.v1:", '6'),
        id("cognition.local-goose-dialogue.responder-intent.v1:", '7'),
        id("cognition.local-goose-dialogue.responder-disposition.v1:", '8'),
        "",
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

    const auto first = lineage("retry-sequence4-generation1", 1000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(first));
    assert(first.retry_generation == 1);
    assert(first.historical_grant_terminal_reason.empty());

    const auto first_replay = lineage("retry-sequence4-generation1", 2000);
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
    assert(store.find_for_event(first.consumer_id, first.event_id));

    const auto same_request_changed_evidence = lineage(
        first.request_id, 1100,
        first.event_id, first.completion_sequence,
        "different_terminal_reason");
    assert(same_request_changed_evidence.lineage_id != first.lineage_id);
    assert(store.prepare_lineage(same_request_changed_evidence).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto same_event = lineage(
        "retry-sequence4-event-conflict", 1200);
    assert(store.prepare_lineage(same_event).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    const auto same_sequence = lineage(
        "retry-sequence4-sequence-conflict", 1300,
        id("cognition.local-goose-dialogue.completion.v1:", 'a'), 4);
    assert(store.prepare_lineage(same_sequence).result
        == LocalGooseDialogueResponderRetryStoreResult::conflict);

    auto forbidden_second_generation = lineage(
        "retry-sequence4-generation2", 2000,
        id("cognition.local-goose-dialogue.completion.v1:", 'b'), 5);
    forbidden_second_generation.retry_generation = 2;
    assert(!valid_local_goose_dialogue_responder_retry_lineage(
        forbidden_second_generation));
    assert(store.prepare_lineage(forbidden_second_generation).result
        == LocalGooseDialogueResponderRetryStoreResult::invalid);

    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_retry_lineage "
        "WHERE retry_generation=1;") == 1);

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

    // A strict existing-only reader cannot initialize, migrate or mutate the
    // lineage sidecar, even if prepare_lineage is called through the same class.
    const auto before_bytes = byte_snapshot(path);
    {
        LocalGooseDialogueResponderRetryStore reader(
            path, LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
        assert(reader.is_read_only());
        assert(reader.schema_version() == 1);
        assert(reader.find_lineage(first.lineage_id));
        assert(reader.find_for_event(first.consumer_id, first.event_id));
        assert(reader.find_by_request(first.request_id));
        assert(reader.prepare_lineage(first).result
            == LocalGooseDialogueResponderRetryStoreResult::unavailable);
    }
    assert(!store.is_read_only());
    assert(byte_snapshot(path) == before_bytes);
    assert(!std::filesystem::exists(path + "-wal"));
    assert(!std::filesystem::exists(path + "-shm"));
    assert(!std::filesystem::exists(path + "-journal"));

    // Never create an absent file (the legacy writer constructor may create).
    const auto absent = temp_path("strict-absent");
    std::filesystem::remove(absent);
    bool rejected_absent = false;
    try {
        LocalGooseDialogueResponderRetryStore reader(
            absent, LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
    } catch (const std::exception&) {
        rejected_absent = true;
    }
    assert(rejected_absent && !std::filesystem::exists(absent));

    // Symlink and wrong mode are rejected before SQLite can open the file.
    const auto link_path = temp_path("strict-link");
    const auto unsafe_path = temp_path("strict-unsafe");
    std::filesystem::remove(link_path);
    std::filesystem::remove(unsafe_path);
    std::filesystem::create_symlink(path, link_path);
    std::filesystem::copy_file(path, unsafe_path);
    assert(chmod(unsafe_path.c_str(), 0644) == 0);
    for (const auto& forbidden : {link_path, unsafe_path}) {
        bool rejected = false;
        try {
            LocalGooseDialogueResponderRetryStore reader(
                forbidden,
                LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
        } catch (const std::exception&) {
            rejected = true;
        }
        assert(rejected);
    }
    assert(byte_snapshot(path) == before_bytes);
    std::filesystem::remove(link_path);
    std::filesystem::remove(unsafe_path);

    // An active companion must never be recovered by a "read-only" snapshot.
    {
        std::ofstream wal(path + "-wal");
        wal << "fake WAL companion";
    }
    bool rejected_companion = false;
    try {
        LocalGooseDialogueResponderRetryStore reader(
            path, LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
    } catch (const std::exception&) {
        rejected_companion = true;
    }
    assert(rejected_companion);
    std::filesystem::remove(path + "-wal");
    assert(byte_snapshot(path) == before_bytes);

    // Reject an unversioned file, not silently turn it into schema v1.
    const auto unversioned = temp_path("strict-unversioned");
    std::filesystem::remove(unversioned);
    sqlite3* empty_db = nullptr;
    assert(sqlite3_open_v2(unversioned.c_str(), &empty_db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                           nullptr) == SQLITE_OK);
    assert(sqlite3_close(empty_db) == SQLITE_OK);
    assert(chmod(unversioned.c_str(), 0600) == 0);
    const auto unversioned_bytes = byte_snapshot(unversioned);
    bool rejected_unversioned = false;
    try {
        LocalGooseDialogueResponderRetryStore reader(
            unversioned,
            LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
    } catch (const std::exception&) {
        rejected_unversioned = true;
    }
    assert(rejected_unversioned);
    assert(byte_snapshot(unversioned) == unversioned_bytes);
    std::filesystem::remove(unversioned);

    const auto future_bytes = byte_snapshot(future_path);
    bool rejected_readonly_future = false;
    try {
        LocalGooseDialogueResponderRetryStore reader(
            future_path,
            LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only);
    } catch (const std::exception&) {
        rejected_readonly_future = true;
    }
    assert(rejected_readonly_future);
    assert(byte_snapshot(future_path) == future_bytes);

    std::filesystem::remove(path);
    std::filesystem::remove(future_path);
    std::cout << "local_goose_dialogue_responder_retry_store_test: PASS\n";
    return 0;
}
