#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace gaudere_agent;

namespace {

std::string temp_path(const std::string& suffix)
{
    return "/tmp/gaudere-responder-schema3-reopen-"
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

LocalGooseDialogueResponderLease lease(
    const std::string& id,
    const std::string& thread,
    const std::int64_t issued)
{
    LocalGooseDialogueResponderLease out;
    out.lease_id = id;
    out.thread_alias = thread;
    out.speaker_id = "sol";
    out.allowed_message_kinds =
        local_goose_dialogue_responder_message_feedback;
    out.purpose = "schema3 reopen compatibility proof";
    out.max_system_turns = 1;
    out.issued_at_ms = issued;
    out.expires_at_ms = issued + 10000;
    out.min_interval_ms = 0;
    out.next_eligible_at_ms = issued;
    return out;
}

} // namespace

int main()
{
    const auto path = temp_path("main");
    const auto future = temp_path("future");
    std::filesystem::remove(path);
    std::filesystem::remove(future);

    // Base store remains the owner/creator of schema v2 only.
    {
        LocalGooseDialogueResponderStore base(path);
        assert(scalar(path, "PRAGMA user_version;")
            == local_goose_dialogue_responder_owned_sidecar_schema);
        assert(local_goose_dialogue_responder_owned_sidecar_schema == 2);
        assert(local_goose_dialogue_responder_sidecar_schema == 3);
        const auto first = lease("compat-main", "main", 1000);
        assert(base.create_lease(first).result
            == LocalGooseDialogueResponderStoreResult::accepted);
    }
    assert(scalar(path, "PRAGMA user_version;") == 2);

    // Only AuthorityStore has explicit v2 -> v3 migration authority.
    std::string grant_id;
    {
        LocalGooseDialogueResponderAuthorityStore authority(
            path,
            LocalGooseDialogueResponderAuthorityStoreOpenMode::allow_schema_upgrade);
        assert(authority.schema_version() == 3);
        assert(scalar(path, "PRAGMA user_version;") == 3);

        const auto grant = make_local_goose_dialogue_responder_grant(
            "schema3-reopen-grant",
            "local-typed-operator-v1",
            "main",
            "sol",
            local_goose_dialogue_responder_message_feedback,
            "schema3 reopen compatibility proof",
            1,
            5000,
            0,
            900,
            20000);
        assert(valid_local_goose_dialogue_responder_grant(grant));
        assert(authority.prepare_grant(grant).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        grant_id = grant.grant_id;
    }
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;") == 0);

    // Process-style restart: a fresh base responder store can reopen schema v3.
    // It may operate on its owned v2 tables but does not own/mutate authority
    // tables and cannot perform the v2 -> v3 migration itself.
    {
        LocalGooseDialogueResponderStore reopened(path);
        const auto historical = reopened.find_lease("compat-main");
        assert(historical && historical->thread_alias == "main");

        const auto second = lease("compat-aux", "aux", 2000);
        assert(reopened.create_lease(second).result
            == LocalGooseDialogueResponderStoreResult::accepted);
        assert(reopened.find_lease("compat-aux"));
    }
    assert(scalar(path, "PRAGMA user_version;") == 3);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;") == 1);
    assert(scalar(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;") == 0);

    // AuthorityStore remains the schema-v3 integrity validator after restart.
    {
        LocalGooseDialogueResponderAuthorityStore authority(path);
        assert(authority.schema_version() == 3);
        const auto persisted = authority.find_grant(grant_id);
        assert(persisted && persisted->request_id == "schema3-reopen-grant");
    }

    // A brand-new base sidecar still starts at v2, never v3.
    {
        LocalGooseDialogueResponderStore base(future);
        assert(scalar(future, "PRAGMA user_version;") == 2);
    }

    // Unknown future schema remains fail-closed for the base store.
    execute_sql(future, "PRAGMA user_version=4;");
    bool future_rejected = false;
    try {
        LocalGooseDialogueResponderStore unsupported(future);
    } catch (const std::exception&) {
        future_rejected = true;
    }
    assert(future_rejected);

    std::filesystem::remove(path);
    std::filesystem::remove(future);
    std::cout << "local_goose_dialogue_responder_schema3_reopen_test: PASS\n";
    return 0;
}
