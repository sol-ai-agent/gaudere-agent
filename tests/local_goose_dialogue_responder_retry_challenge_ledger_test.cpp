#include "LocalGooseDialogueResponderRetryChallengeLedger.hpp"

#include <sqlite3.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>

namespace {

using namespace gaudere_agent;
using Ledger = LocalGooseDialogueResponderRetryChallengeLedger;
using Mode = LocalGooseDialogueResponderRetryChallengeLedgerOpenMode;
using Code = LocalGooseDialogueResponderRetryChallengeLedgerCode;
using State = LocalGooseDialogueResponderRetryChallengeState;
using Reservation = LocalGooseDialogueResponderRetryChallengeReservation;

struct Temp {
    explicit Temp(const std::string& tag)
    {
        static int serial = 0;
        path = std::filesystem::temp_directory_path()
            / ("gaudere-retry-challenge-" + tag + "-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".sqlite");
    }
    ~Temp()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        for (const char* suffix : {"-wal", "-shm", "-journal"})
            std::filesystem::remove(path.string() + suffix, ec);
    }
    std::filesystem::path path;
};

std::string id(const char* prefix, const char c)
{
    return std::string(prefix) + std::string(64, c);
}

Reservation record(const char nonce_char = 'a',
                   const char challenge_char = 'b',
                   const char scope_char = 'c',
                   const char request_suffix = '1')
{
    const auto r = make_local_goose_dialogue_responder_retry_challenge_reservation(
        std::string("reserve-request-") + request_suffix,
        id("cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:",
           challenge_char),
        id("cognition.local-goose-dialogue.responder-retry-grant-proposal.v1:", 'd'),
        id("cognition.local-goose-dialogue.responder-retry-scope.v1:", scope_char),
        id("cognition.local-goose-dialogue.responder-retry-lineage.v1:", 'e'),
        std::string(64, nonce_char), 4000, 14000);
    assert(r);
    assert(valid_local_goose_dialogue_responder_retry_challenge_reservation(*r));
    return *r;
}

std::string bytes(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    assert(f.good());
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

bool rejects(const std::filesystem::path& p, const Mode mode)
{
    try { Ledger l(p.string(), mode); }
    catch (const std::exception&) { return true; }
    return false;
}

void exec_sql(const std::filesystem::path& path, const char* sql)
{
    sqlite3* db = nullptr;
    assert(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    assert(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

void test_reserve_burn_and_restart()
{
    Temp t("restart");
    const auto r = record();
    {
        Ledger l(t.path.string(), Mode::create_new_isolated);
        assert(l.schema_version() == 1);
        assert(!l.is_read_only());
        assert(l.reserve(r) == Code::accepted);
        assert(l.reserve(r) == Code::exact_duplicate_reserved);
        auto row = l.find_challenge(r.challenge_id);
        assert(row && row->state == State::reserved);
        assert(!row->evidence_sha256);
        assert(l.burn(r.challenge_id, "wrong-request", std::string(64, 'f'))
            == Code::conflict);
        assert(l.burn(r.challenge_id, r.request_id, "bad-hash")
            == Code::invalid);
        assert(l.burn(r.challenge_id, r.request_id, std::string(64, 'f'))
            == Code::accepted);
        assert(l.burn(r.challenge_id, r.request_id, std::string(64, 'f'))
            == Code::conflict);
        assert(l.mark_uncertain(r.challenge_id, r.request_id)
            == Code::conflict);
        assert(l.reserve(r) == Code::conflict);
    }
    {
        Ledger l(t.path.string(), Mode::existing_writer);
        const auto row = l.find_challenge(r.challenge_id);
        assert(row && row->state == State::burned);
        assert(row->evidence_sha256 == std::string(64, 'f'));
        assert(l.burn(r.challenge_id, r.request_id, std::string(64, 'f'))
            == Code::conflict);
        assert(l.reserve(r) == Code::conflict);
    }
    const auto original = bytes(t.path);
    {
        Ledger l(t.path.string(), Mode::existing_read_only);
        assert(l.is_read_only());
        assert(l.find_challenge(r.challenge_id)->state == State::burned);
        assert(l.reserve(r) == Code::unavailable);
        assert(l.burn(r.challenge_id, r.request_id, std::string(64, 'f'))
            == Code::unavailable);
        assert(l.mark_uncertain(r.challenge_id, r.request_id)
            == Code::unavailable);
    }
    assert(bytes(t.path) == original);
}

void test_unique_nonce_and_generation_budget()
{
    Temp t("unique");
    const auto r = record();
    Ledger l(t.path.string(), Mode::create_new_isolated);
    assert(l.reserve(r) == Code::accepted);

    // A distinct challenge may never reuse a nonce fingerprint.
    const auto same_nonce = record('a', 'f', 'e', '2');
    assert(l.reserve(same_nonce) == Code::conflict);
    // A second challenge against the exact scope/generation is also denied.
    const auto same_scope = record('1', 'f', 'c', '3');
    assert(l.reserve(same_scope) == Code::conflict);
    // A changed request collides against exact original identity.
    auto mutated = r;
    mutated.request_id = "other-request";
    assert(l.reserve(mutated) == Code::invalid);
    mutated = r;
    mutated.retry_generation = 2;
    assert(l.reserve(mutated) == Code::invalid);
    mutated = r;
    mutated.expires_at_ms = 300001 + mutated.issued_at_ms;
    assert(l.reserve(mutated) == Code::invalid);
    mutated = r;
    mutated.challenge_id = id(
        "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:", '0');
    assert(l.reserve(mutated) == Code::invalid);

    // Distinct nonces, challenges and scopes can coexist on a separate scope
    // (this policy does NOT authorize any additional production retry).
    assert(l.reserve(record('2', '3', '4', '4')) == Code::accepted);
}

void test_absorbing_uncertain()
{
    Temp t("uncertain");
    const auto r = record();
    {
        Ledger l(t.path.string(), Mode::create_new_isolated);
        assert(l.reserve(r) == Code::accepted);
        assert(l.mark_uncertain(r.challenge_id, r.request_id)
            == Code::accepted);
        assert(l.mark_uncertain(r.challenge_id, r.request_id)
            == Code::conflict);
        assert(l.burn(r.challenge_id, r.request_id, std::string(64, 'b'))
            == Code::conflict);
    }
    {
        Ledger l(t.path.string(), Mode::existing_read_only);
        assert(l.find_challenge(r.challenge_id)->state == State::manual_review);
        assert(!l.find_challenge(r.challenge_id)->evidence_sha256);
    }
}

void test_absent_schema_permission_companions()
{
    Temp t("safety");
    assert(rejects(t.path, Mode::existing_read_only));
    assert(rejects(t.path, Mode::existing_writer));
    assert(!std::filesystem::exists(t.path));

    {
        Ledger l(t.path.string(), Mode::create_new_isolated);
        assert(l.schema_version() == 1);
    }
    assert(rejects(t.path, Mode::create_new_isolated));

    assert(chmod(t.path.string().c_str(), 0644) == 0);
    assert(rejects(t.path, Mode::existing_read_only));
    assert(rejects(t.path, Mode::existing_writer));
    assert(chmod(t.path.string().c_str(), 0600) == 0);

    Temp link("symlink");
    std::filesystem::create_symlink(t.path, link.path);
    assert(rejects(link.path, Mode::existing_read_only));

    std::ofstream(t.path.string() + "-wal") << "unexpected companion";
    assert(rejects(t.path, Mode::existing_read_only));
    assert(rejects(t.path, Mode::existing_writer));
    std::filesystem::remove(t.path.string() + "-wal");

    std::ofstream(t.path.string() + "-journal") << "possible interrupted write";
    assert(rejects(t.path, Mode::existing_read_only));
    std::filesystem::remove(t.path.string() + "-journal");

    exec_sql(t.path, "PRAGMA user_version=2");
    assert(rejects(t.path, Mode::existing_read_only));
    assert(rejects(t.path, Mode::existing_writer));

    Temp foreign("foreign");
    exec_sql(foreign.path,
        "PRAGMA user_version=1;"
        "CREATE TABLE local_goose_dialogue_responder_retry_challenge_reservation("
        "reservation_id TEXT PRIMARY KEY)");
    assert(chmod(foreign.path.string().c_str(), 0600) == 0);
    assert(rejects(foreign.path, Mode::existing_read_only));
}

void test_invalid_shape()
{
    auto r = record();
    r.request_id = std::string(129, 'x');
    assert(!valid_local_goose_dialogue_responder_retry_challenge_reservation(r));
    const auto invalid = make_local_goose_dialogue_responder_retry_challenge_reservation(
        "request-one", "not-a-challenge", r.proposal_id, r.scope_id,
        r.lineage_id, r.nonce_sha256, 100, 101);
    assert(!invalid);
    const auto overlong = make_local_goose_dialogue_responder_retry_challenge_reservation(
        "request-one", record().challenge_id, record().proposal_id,
        record().scope_id, record().lineage_id, std::string(64, 'e'), 0, 300001);
    assert(!overlong);
}

} // namespace

int main()
{
    test_reserve_burn_and_restart();
    test_unique_nonce_and_generation_budget();
    test_absorbing_uncertain();
    test_absent_schema_permission_companions();
    test_invalid_shape();
    return 0;
}
