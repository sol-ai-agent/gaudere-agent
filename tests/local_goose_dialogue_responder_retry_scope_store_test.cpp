#include "LocalGooseDialogueResponderRetryScopeStore.hpp"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>

namespace {

using namespace gaudere_agent;
using Store = LocalGooseDialogueResponderRetryScopeStore;
using Mode = LocalGooseDialogueResponderRetryScopeOpenMode;
using Result = LocalGooseDialogueResponderRetryScopeResult;

struct Temporary {
    explicit Temporary(const std::string& label)
    {
        static unsigned counter = 0;
        path = std::filesystem::temp_directory_path()
            / ("gaudere-retry-scope-" + label + "-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + "-" + std::to_string(++counter) + ".db");
    }
    ~Temporary()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path.string() + "-wal", ec);
        std::filesystem::remove(path.string() + "-shm", ec);
        std::filesystem::remove(path.string() + "-journal", ec);
    }
    std::filesystem::path path;
};

std::string hash_id(const std::string& prefix, char c)
{
    return prefix + std::string(64, c);
}

LocalGooseDialogueResponderRetryLineage lineage(
    const std::string& request = "lineage-request-1",
    const char event_hex = 'a')
{
    auto l = make_local_goose_dialogue_responder_retry_lineage(
        request, "system-responder-v1", "main", 4,
        hash_id("cognition.local-goose-dialogue.completion.v1:", event_hex),
        std::string(64, 'b'), 3,
        hash_id("cognition.local-goose-dialogue.v3:", 'c'),
        "bertrand",
        hash_id("cognition.local-goose-dialogue.responder-grant.v1:", 'd'),
        "historical-lease-1",
        hash_id("cognition.local-goose-dialogue.responder-decision.v1:", 'e'),
        hash_id("cognition.local-goose-dialogue.responder-intent.v1:", 'f'),
        hash_id("cognition.local-goose-dialogue.responder-disposition.v1:", '1'),
        "", "lease_expired", "bound_intent_expired_before_dispatch",
        "lease_or_intent_expired", "lease_or_intent_expired", 1000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(l));
    return l;
}

LocalGooseDialogueResponderRetryScope make_scope(
    const std::string& request = "scope-request-1",
    const char event_hex = 'a')
{
    const auto s = make_local_goose_dialogue_responder_retry_scope(
        request, lineage("lineage-request-1", event_hex), 2000);
    assert(s);
    assert(valid_local_goose_dialogue_responder_retry_scope(*s));
    return *s;
}

std::string bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    assert(input.good());
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

bool rejected(const std::filesystem::path& path, const Mode mode)
{
    try {
        Store store(path.string(), mode);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void sql_exec(const std::filesystem::path& path, const char* sql)
{
    sqlite3* database = nullptr;
    assert(sqlite3_open(path.string().c_str(), &database) == SQLITE_OK);
    assert(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(database) == SQLITE_OK);
}

void test_exact_replay_and_conflict()
{
    Temporary tmp("replay");
    const auto scope = make_scope();
    {
        Store store(tmp.path.string(), Mode::create_new_isolated);
        assert(store.schema_version() == 1);
        assert(!store.is_read_only());
        assert(store.stage_scope(scope).result == Result::accepted);
        const auto same = store.stage_scope(scope);
        assert(same.result == Result::duplicate);
        assert(same.scope && same.scope->scope_id == scope.scope_id);
        assert(store.find_by_lineage(scope.lineage_id)->scope_id == scope.scope_id);
        assert(store.find_by_request(scope.request_id)->scope_id == scope.scope_id);

        // Same lineage, different scope request: never replace one attempt.
        auto second = make_scope("scope-request-2");
        assert(store.stage_scope(second).result == Result::conflict);

        // Same scope request, different lineage/event: strict idempotence.
        second = make_scope("scope-request-1", '2');
        assert(store.stage_scope(second).result == Result::conflict);

        // Same consumer/event/generation with distinct canonical lineage.
        const auto alternate = make_local_goose_dialogue_responder_retry_scope(
            "scope-request-3", lineage("other-lineage-request"), 2100);
        assert(alternate);
        assert(store.stage_scope(*alternate).result == Result::conflict);

        auto invalid = scope;
        invalid.retry_generation = 2;
        assert(store.stage_scope(invalid).result == Result::invalid);
        invalid = scope;
        invalid.completion_sequence = 5;
        assert(store.stage_scope(invalid).result == Result::invalid);
        invalid = scope;
        invalid.observed_speaker_kind = "system";
        assert(store.stage_scope(invalid).result == Result::invalid);
    }
    {
        Store store(tmp.path.string(), Mode::existing_writer);
        assert(store.stage_scope(scope).result == Result::duplicate);
    }
    const auto before = bytes(tmp.path);
    {
        Store store(tmp.path.string(), Mode::existing_read_only);
        assert(store.is_read_only());
        assert(store.find_by_request(scope.request_id)->scope_id == scope.scope_id);
        assert(store.stage_scope(scope).result == Result::unavailable);
    }
    assert(bytes(tmp.path) == before);
}

void test_missing_and_unsafe_open()
{
    Temporary tmp("missing");
    assert(rejected(tmp.path, Mode::existing_read_only));
    assert(rejected(tmp.path, Mode::existing_writer));
    assert(!std::filesystem::exists(tmp.path));

    {
        Store store(tmp.path.string(), Mode::create_new_isolated);
        assert(store.schema_version() == 1);
    }
    assert(rejected(tmp.path, Mode::create_new_isolated));
    assert(chmod(tmp.path.string().c_str(), 0644) == 0);
    assert(rejected(tmp.path, Mode::existing_read_only));
    assert(rejected(tmp.path, Mode::existing_writer));
    assert(chmod(tmp.path.string().c_str(), 0600) == 0);

    Temporary alias("symlink");
    std::filesystem::create_symlink(tmp.path, alias.path);
    assert(rejected(alias.path, Mode::existing_read_only));
    assert(rejected(alias.path, Mode::existing_writer));

    {
        std::ofstream(tmp.path.string() + "-wal") << "not a quiescent SQLite file";
        assert(rejected(tmp.path, Mode::existing_read_only));
        assert(rejected(tmp.path, Mode::existing_writer));
        std::filesystem::remove(tmp.path.string() + "-wal");
    }
    {
        std::ofstream(tmp.path.string() + "-journal") << "hot journal";
        assert(rejected(tmp.path, Mode::existing_read_only));
        std::filesystem::remove(tmp.path.string() + "-journal");
    }
}

void test_foreign_and_future_schema()
{
    Temporary foreign("foreign");
    sql_exec(foreign.path,
        "PRAGMA user_version=1;"
        "CREATE TABLE local_goose_dialogue_responder_retry_scope("
        "scope_id TEXT PRIMARY KEY)");
    assert(chmod(foreign.path.string().c_str(), 0600) == 0);
    assert(rejected(foreign.path, Mode::existing_read_only));
    assert(rejected(foreign.path, Mode::existing_writer));

    Temporary future("future");
    {
        Store store(future.path.string(), Mode::create_new_isolated);
        assert(store.schema_version() == 1);
    }
    sql_exec(future.path, "PRAGMA user_version=2");
    assert(rejected(future.path, Mode::existing_read_only));
    assert(rejected(future.path, Mode::existing_writer));
}

void test_bad_lineage_does_not_stage()
{
    auto l = lineage();
    l.retry_generation = 2;
    assert(!make_local_goose_dialogue_responder_retry_scope(
        "scope-request-3", l, 2000));
    l = lineage();
    l.lineage_id = hash_id(
        "cognition.local-goose-dialogue.responder-retry-lineage.v1:", '0');
    assert(!make_local_goose_dialogue_responder_retry_scope(
        "scope-request-3", l, 2000));
    l = lineage();
    assert(!make_local_goose_dialogue_responder_retry_scope(
        "scope-request-3", l, -1));
}

} // namespace

int main()
{
    test_exact_replay_and_conflict();
    test_missing_and_unsafe_open();
    test_foreign_and_future_schema();
    test_bad_lineage_does_not_stage();
    return 0;
}
