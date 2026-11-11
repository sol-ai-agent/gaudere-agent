#include "LocalGooseDialogueResponderRetryHostChallengeOnceLedger.hpp"
#include "Sha256.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace gaudere_agent;
using Code = LocalGooseDialogueResponderRetryHostOnceCode;
using Ledger = LocalGooseDialogueResponderRetryHostChallengeOnceLedger;
using Anchor = LocalGooseDialogueResponderRetryRootOwnedAnchorRecord;

struct SyntheticHost {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> priv {
        nullptr, EVP_PKEY_free
    };
    explicit SyntheticHost(unsigned char seed_byte)
    {
        // CI-only synthetic key; no operator or real host material.
        std::array<unsigned char, 32> seed {};
        seed.fill(seed_byte);
        priv.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(priv);
        std::size_t len = pub.size();
        assert(EVP_PKEY_get_raw_public_key(priv.get(), pub.data(), &len) == 1);
        assert(len == pub.size());
    }
};

std::string hex(const unsigned char* bytes, std::size_t n)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        result += alphabet[bytes[i] >> 4];
        result += alphabet[bytes[i] & 15];
    }
    return result;
}

std::string sign(SyntheticHost& key, const std::string& exact)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(ctx);
    assert(EVP_DigestSignInit(
        ctx.get(), nullptr, nullptr, nullptr, key.priv.get()) == 1);
    std::array<unsigned char, 64> signature {};
    std::size_t len = signature.size();
    assert(EVP_DigestSign(ctx.get(), signature.data(), &len,
        reinterpret_cast<const unsigned char*>(exact.data()),
        exact.size()) == 1);
    assert(len == signature.size());
    return hex(signature.data(), len);
}

std::string host_fingerprint(const SyntheticHost& host)
{
    const std::string bytes(
        reinterpret_cast<const char*>(host.pub.data()), host.pub.size());
    return sha256_hex("gaudere-retry-host-attestor-ed25519-key-v1|" + bytes);
}

Anchor anchor()
{
    Anchor a;
    a.root_pin_path = "/tmp/gaudere-once-synthetic-private/root.pin";
    a.root_fingerprint = std::string(64, 'a');
    a.operator_key_fingerprint = std::string(64, 'b');
    a.anchor_epoch = 7;
    return a;
}

std::string signed_v1(SyntheticHost& host, const Anchor& a,
                      const std::string& context)
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_host_receipt_header)
        + "root_pin_path=" + a.root_pin_path + "\n"
        + "root_fingerprint=" + a.root_fingerprint + "\n"
        + "operator_key_fingerprint=" + a.operator_key_fingerprint + "\n"
        + "anchor_epoch=7\n"
        + "host_context_sha256=" + context + "\n"
        + "not_before_ms=1000\n"
        + "not_after_ms=2000000\n";
    return body + "signature_hex=" + sign(host,
        std::string(local_goose_dialogue_responder_retry_host_receipt_domain)
            + body) + "\n";
}

std::string signed_v2(SyntheticHost& host, const std::string& v1,
                      const std::string& challenge, const std::string& request)
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_host_challenge_header)
        + "receipt_sha256=" + sha256_hex(v1) + "\n"
        + "challenge_sha256=" + challenge + "\n"
        + "request_context_sha256=" + request + "\n"
        + "not_before_ms=3000\n"
        + "not_after_ms=180000\n";
    return body + "signature_hex=" + sign(host,
        std::string(local_goose_dialogue_responder_retry_host_challenge_domain)
            + body) + "\n";
}

void exec(sqlite3* db, const char* sql)
{
    assert(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}

void insert_synthetic_challenge(
    sqlite3* db, const std::string& challenge, const std::string& request,
    const std::string& context, std::uint64_t epoch, std::int64_t expiry)
{
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO local_goose_dialogue_responder_retry_host_challenge_once"
        "(challenge_sha256,request_context_sha256,host_context_sha256,"
        "anchor_epoch,expires_ms,consumed,accepted_proof_sha256)"
        " VALUES(?1,?2,?3,?4,?5,0,'')", -1, &stmt, nullptr) == SQLITE_OK);
    assert(sqlite3_bind_text(stmt, 1, challenge.c_str(), -1, SQLITE_TRANSIENT)
        == SQLITE_OK);
    assert(sqlite3_bind_text(stmt, 2, request.c_str(), -1, SQLITE_TRANSIENT)
        == SQLITE_OK);
    assert(sqlite3_bind_text(stmt, 3, context.c_str(), -1, SQLITE_TRANSIENT)
        == SQLITE_OK);
    assert(sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(epoch))
        == SQLITE_OK);
    assert(sqlite3_bind_int64(stmt, 5, expiry) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
}

struct Temp {
    std::filesystem::path dir, db;
    Temp()
    {
        dir = std::filesystem::temp_directory_path() /
            ("gaudere-once-ledger-test-"
             + std::to_string(getpid()) + "-"
             + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::create_directory(dir));
        assert(chmod(dir.c_str(), 0700) == 0);
        db = dir / "once.db";
    }
    ~Temp()
    {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
};

void bootstrap(const Temp& temp)
{
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2(temp.db.c_str(), &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) == SQLITE_OK);
    exec(db, "PRAGMA journal_mode=DELETE");
    exec(db, "PRAGMA synchronous=FULL");
    exec(db, local_goose_dialogue_responder_retry_host_once_schema_sql);
    exec(db, "PRAGMA user_version=1");

    const std::string request(64, 'e'), context(64, 'c');
    insert_synthetic_challenge(db, std::string(64, 'd'), request, context, 7, 9000);
    insert_synthetic_challenge(db, std::string(64, 'f'), request, context, 7, 7000);
    insert_synthetic_challenge(db, std::string(64, '1'), request, context, 7, 9000);
    insert_synthetic_challenge(db, std::string(64, '2'),
                               std::string(64, '3'), context, 7, 9000);
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(chmod(temp.db.c_str(), 0600) == 0);
}

Code check(Ledger& ledger, const std::string& v1, const std::string& v2,
           const SyntheticHost& host, const std::string& challenge,
           const std::string& request = std::string(64, 'e'),
           const std::int64_t now = 5000)
{
    return verify_and_mark_local_goose_dialogue_responder_retry_host_challenge_once(
        ledger, v1, v2, host.pub, host_fingerprint(host),
        std::string(64, 'c'), anchor(), 7, challenge, request, now);
}

void test_one_commit_restart_replay_wrong_request_and_expiry()
{
    Temp temp;
    bootstrap(temp);
    SyntheticHost host(0x36), foreign(0x37);
    const auto v1 = signed_v1(host, anchor(), std::string(64, 'c'));
    const auto challenge = std::string(64, 'd');
    const auto request = std::string(64, 'e');
    const auto v2 = signed_v2(host, v1, challenge, request);

    {
        Ledger ledger(temp.db.string());
        const auto forged = signed_v2(foreign, v1, challenge, request);
        assert(check(ledger, v1, forged, host, challenge)
            == Code::invalid_signed_proof_no_authority);
        assert(check(ledger, v1, v2, host, std::string(64, '2'))
            == Code::invalid_signed_proof_no_authority);
        assert(check(ledger, v1, v2, host, challenge)
            == Code::verified_challenge_marked_consumed_no_authority);
        assert(check(ledger, v1, v2, host, challenge)
            == Code::missing_or_already_consumed_no_authority);
    }
    {
        // Opening existing database after a normal restart never reissues.
        Ledger restarted(temp.db.string());
        assert(check(restarted, v1, v2, host, challenge)
            == Code::missing_or_already_consumed_no_authority);

        const auto second = std::string(64, 'f');
        const auto second_v2 = signed_v2(host, v1, second, request);
        assert(check(restarted, v1, second_v2, host, second, request, 8000)
            == Code::mismatched_or_expired_issued_challenge_no_authority);
        assert(check(restarted, v1, second_v2, host, second)
            == Code::verified_challenge_marked_consumed_no_authority);

        const auto wrong_record = std::string(64, '2');
        const auto wrong_v2 = signed_v2(host, v1, wrong_record, request);
        assert(check(restarted, v1, wrong_v2, host, wrong_record)
            == Code::mismatched_or_expired_issued_challenge_no_authority);
        assert(check(restarted, v1, signed_v2(
                   host, v1, wrong_record, std::string(64, '3')),
                   host, wrong_record, std::string(64, '3'))
            == Code::verified_challenge_marked_consumed_no_authority);
    }

    // No implicit CREATE on a missing database or unsafe path.
    bool threw = false;
    try { Ledger missing((temp.dir / "absent.db").string()); }
    catch (...) { threw = true; }
    assert(threw);
    assert(!std::filesystem::exists(temp.dir / "absent.db"));
}

void test_two_independent_processes_compete_for_one_challenge()
{
    Temp temp;
    bootstrap(temp);
    SyntheticHost host(0x36);
    const auto v1 = signed_v1(host, anchor(), std::string(64, 'c'));
    const std::string challenge(64, '1'), request(64, 'e');
    const auto v2 = signed_v2(host, v1, challenge, request);
    int ready_pipe[2], release_pipe[2], outcome_pipe[2];
    assert(pipe(ready_pipe) == 0);
    assert(pipe(release_pipe) == 0);
    assert(pipe(outcome_pipe) == 0);
    pid_t children[2] {};
    for (int i = 0; i < 2; ++i) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (children[i] == 0) {
            close(ready_pipe[0]);
            close(release_pipe[1]);
            close(outcome_pipe[0]);
            // Each process uses its OWN SQLite connection, opened pre-lock.
            Ledger ledger(temp.db.string());
            const char ready = 'r';
            assert(write(ready_pipe[1], &ready, 1) == 1);
            char release {};
            assert(read(release_pipe[0], &release, 1) == 1);
            const auto result = check(ledger, v1, v2, host, challenge);
            const unsigned char encoded = static_cast<unsigned char>(result);
            assert(write(outcome_pipe[1], &encoded, 1) == 1);
            _exit(0);
        }
    }
    close(ready_pipe[1]);
    close(release_pipe[0]);
    close(outcome_pipe[1]);
    char ready[2] {};
    assert(read(ready_pipe[0], &ready[0], 1) == 1);
    assert(read(ready_pipe[0], &ready[1], 1) == 1);
    assert(write(release_pipe[1], "go", 2) == 2);
    unsigned char results[2] {};
    assert(read(outcome_pipe[0], &results[0], 1) == 1);
    assert(read(outcome_pipe[0], &results[1], 1) == 1);
    for (const pid_t child : children) {
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    close(ready_pipe[0]);
    close(release_pipe[1]);
    close(outcome_pipe[0]);
    int committed = 0, consumed = 0;
    for (const auto result : results) {
        if (result == static_cast<unsigned char>(
                Code::verified_challenge_marked_consumed_no_authority))
            ++committed;
        if (result == static_cast<unsigned char>(
                Code::missing_or_already_consumed_no_authority))
            ++consumed;
    }
    assert(committed == 1 && consumed == 1);

    Ledger restarted(temp.db.string());
    assert(check(restarted, v1, v2, host, challenge)
        == Code::missing_or_already_consumed_no_authority);
}

} // namespace

int main()
{
    test_one_commit_restart_replay_wrong_request_and_expiry();
    test_two_independent_processes_compete_for_one_challenge();
    return 0;
}
