#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"
#include "LocalGooseDialogueResponderRetryOperatorTrustFloorLedger.hpp"
#include "LocalGooseDialogueResponderRetryProofReservationInspector.hpp"
#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"
#include "LocalGooseDialogueResponder.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <atomic>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace {

using namespace gaudere_agent;
using Scope = LocalGooseDialogueResponderRetryScope;
using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Challenge = LocalGooseDialogueResponderRetryOperatorChallenge;
using Review = LocalGooseDialogueResponderRetryExactMessageReview;
using Signed = LocalGooseDialogueResponderRetrySignedExactMessageProof;
using Code = LocalGooseDialogueResponderRetryExactMessageProofCode;

std::string id(const char* prefix, const char digit)
{
    return std::string(prefix) + std::string(64, digit);
}

Scope scope()
{
    const auto lineage = make_local_goose_dialogue_responder_retry_lineage(
        "lineage-request-1", "system-responder-v1", "main", 4,
        id("cognition.local-goose-dialogue.completion.v1:", 'a'),
        std::string(64, 'b'), 3,
        id("cognition.local-goose-dialogue.v3:", 'c'),
        "bertrand",
        id("cognition.local-goose-dialogue.responder-grant.v1:", 'd'),
        "historical-lease-1",
        id("cognition.local-goose-dialogue.responder-decision.v1:", 'e'),
        id("cognition.local-goose-dialogue.responder-intent.v1:", 'f'),
        id("cognition.local-goose-dialogue.responder-disposition.v1:", '1'),
        "", "lease_expired", "bound_intent_expired_before_dispatch",
        "lease_or_intent_expired", "lease_or_intent_expired", 1000);
    assert(valid_local_goose_dialogue_responder_retry_lineage(lineage));
    const auto s = make_local_goose_dialogue_responder_retry_scope(
        "scope-request-1", lineage, 2000);
    assert(s);
    return *s;
}

Proposal proposal(const Scope& s)
{
    auto p = make_local_goose_dialogue_responder_retry_grant_proposal(
        s, "proposal-request-1", "approval-ref-1",
        "Review exactly one final retry intervention", 3000, 300000, 120000);
    assert(p);
    return *p;
}

Challenge challenge(const Proposal& p, const Scope& s)
{
    const auto c = make_local_goose_dialogue_responder_retry_operator_challenge(
        p, s, std::string(64, 'a'), 4000, 10000);
    assert(c);
    return *c;
}

Review review(const Proposal& p, const Scope& s, const Challenge& c,
              const std::string& message)
{
    const auto r = make_local_goose_dialogue_responder_retry_exact_message_review(
        p, s, c, message);
    assert(r);
    assert(valid_local_goose_dialogue_responder_retry_exact_message_review(
        *r, p, s, c));
    return *r;
}

struct SyntheticKey {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key {
        nullptr, EVP_PKEY_free
    };
    explicit SyntheticKey(const unsigned char b)
    {
        // Test-only deterministic seed, NEVER an enrolled operator key.
        std::array<unsigned char, 32> seed {};
        seed.fill(b);
        private_key.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(private_key);
        std::size_t n = pub.size();
        assert(EVP_PKEY_get_raw_public_key(
            private_key.get(), pub.data(), &n) == 1);
        assert(n == pub.size());
    }
};

std::string hex_signature(const unsigned char* data, const std::size_t n)
{
    const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += digits[(data[i] >> 4) & 15];
        out += digits[data[i] & 15];
    }
    return out;
}

std::string sign_payload(SyntheticKey& key, const std::string& payload)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(ctx);
    assert(EVP_DigestSignInit(
        ctx.get(), nullptr, nullptr, nullptr, key.private_key.get()) == 1);
    std::array<unsigned char, 64> signature {};
    std::size_t n = signature.size();
    assert(EVP_DigestSign(ctx.get(), signature.data(), &n,
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size()) == 1);
    assert(n == signature.size());
    return hex_signature(signature.data(), n);
}

Signed sign(const Review& r, const Proposal& p, const Scope& s,
            const Challenge& c, SyntheticKey& key)
{
    Signed proof;
    proof.review_id = r.review_id;
    proof.operator_id = "bertrand";
    proof.decision = "approve_exact_retry_message_v1";
    const std::string pub(
        reinterpret_cast<const char*>(key.pub.data()), key.pub.size());
    proof.key_fingerprint =
        sha256_hex("gaudere-retry-operator-ed25519-key-v1|" + pub);
    const auto payload =
        local_goose_dialogue_responder_retry_exact_message_signing_payload(
            r, p, s, c, proof.operator_id, proof.decision);
    assert(payload);
    proof.signature_hex = sign_payload(key, *payload);
    return proof;
}



using PinCode = LocalGooseDialogueResponderRetryPinnedSignatureCode;


using TrustCode = LocalGooseDialogueResponderRetryTrustSnapshotCode;


using FloorLedger = LocalGooseDialogueResponderRetryTrustFloorLedger;
using FloorMode = LocalGooseDialogueResponderRetryTrustFloorOpenMode;
using FloorCode = LocalGooseDialogueResponderRetryTrustFloorCode;

struct Fixture {
    Scope s = scope();
    Proposal p = proposal(s);
    Challenge c = challenge(p, s);
    Review r = review(p, s, c, u8"Réponse exacte.\nÀ bientôt.");
    SyntheticKey key {0x42};
    SyntheticKey root {0x11};
    Signed proof = sign(r, p, s, c, key);
};

struct Temp {
    explicit Temp(const std::string& tag)
    {
        static std::atomic<unsigned> serial {0};
        dir = std::filesystem::temp_directory_path()
            / ("gaudere-retry-floor-" + tag + "-"
                + std::to_string(std::chrono::steady_clock::now()
                                      .time_since_epoch().count())
                + "-" + std::to_string(++serial));
        assert(std::filesystem::create_directory(dir));
        assert(chmod(dir.string().c_str(), 0700) == 0);
        db = dir / "trust-floor.sqlite";
        pin = dir / "operator.pin";
    }
    ~Temp()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::filesystem::path dir, db, pin;
};

std::string root_fp(const SyntheticKey& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.pub.data()),
                          key.pub.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

void write_pin(const std::filesystem::path& path, const SyntheticKey& key)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.good());
    const auto bytes = std::string(local_goose_dialogue_responder_retry_key_pin_header)
        + hex_signature(key.pub.data(), key.pub.size()) + "\n";
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    assert(out.good());
    assert(chmod(path.string().c_str(), 0400) == 0);
}

std::string snapshot(Fixture& f, const std::string& state,
                     const std::uint64_t generation)
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_trust_snapshot_header)
        + "key_fingerprint=" + f.proof.key_fingerprint + "\n"
        + "status=" + state + "\n"
        + "generation=" + std::to_string(generation) + "\n"
        + "not_before_ms=1000\nnot_after_ms=30000\n";
    return body + "signature_hex="
        + sign_payload(f.root,
            "gaudere-retry-operator-trust-snapshot-signature.v1|" + body)
        + "\n";
}

TrustCode inspect_policy(
    Fixture& f, const Temp& temp,
    const std::string& signed_policy, const std::uint64_t generation)
{
    return inspect_local_goose_dialogue_responder_retry_operator_trust_snapshot(
        signed_policy, f.root.pub, root_fp(f.root), generation, temp.pin.string(),
        f.r, f.p, f.s, f.c, f.proof, 5000);
}

void test_floor_survives_restart_and_blocks_old_root_signed_active()
{
    Fixture f;
    Temp t("restart");
    write_pin(t.pin, f.key);
    const auto trusted_root = root_fp(f.root);
    const auto old_active = snapshot(f, "active", 1);
    const auto newer_revoked = snapshot(f, "revoked", 8);
    {
        FloorLedger bootstrap(t.db.string(), FloorMode::create_new_isolated,
                              trusted_root);
        auto current = bootstrap.inspect(trusted_root);
        assert(current && current->minimum_generation == 1);
        assert(current->last_transition_sha256.size() == 64);
        assert(inspect_policy(f, t, old_active, current->minimum_generation)
            == TrustCode::cryptographically_consistent_no_authority);
        assert(inspect_policy(f, t, newer_revoked, 8)
            == TrustCode::revoked_or_stale_or_outside_validity);

        const auto evidence = sha256_hex(newer_revoked);
        assert(bootstrap.advance_after_external_signature_review(
                   trusted_root, 1, 8, evidence)
            == FloorCode::advanced_no_authority);
        assert(bootstrap.advance_after_external_signature_review(
                   trusted_root, 1, 8, evidence)
            == FloorCode::stale_or_conflicting_floor);
        current = bootstrap.inspect(trusted_root);
        assert(current && current->minimum_generation == 8
            && current->last_transition_sha256 == evidence);
        assert(inspect_policy(f, t, old_active, current->minimum_generation)
            == TrustCode::revoked_or_stale_or_outside_validity);
    }
    // The floor remains visible after closing and re-opening the database.
    {
        FloorLedger reader(t.db.string(), FloorMode::existing_read_only);
        assert(reader.is_read_only());
        const auto current = reader.inspect(trusted_root);
        assert(current && current->minimum_generation == 8);
        assert(inspect_policy(f, t, old_active, current->minimum_generation)
            == TrustCode::revoked_or_stale_or_outside_validity);
        assert(reader.advance_after_external_signature_review(
                   trusted_root, 8, 9, sha256_hex("new-policy"))
            == FloorCode::unavailable_or_commit_uncertain);
    }
    {
        FloorLedger writer(t.db.string(), FloorMode::existing_writer);
        assert(writer.advance_after_external_signature_review(
                   trusted_root, 8, 9, sha256_hex("future-active"))
            == FloorCode::advanced_no_authority);
        assert(writer.inspect(trusted_root)->minimum_generation == 9);
    }
    FloorLedger last(t.db.string(), FloorMode::existing_read_only);
    assert(last.inspect(trusted_root)->minimum_generation == 9);
}

void test_malformed_foreign_root_and_non_monotonic_updates()
{
    Fixture f;
    Temp t("invalid");
    const auto trusted = root_fp(f.root);
    FloorLedger floor(t.db.string(), FloorMode::create_new_isolated, trusted);
    const auto initial = floor.inspect(trusted);
    assert(initial && initial->minimum_generation == 1);
    assert(!floor.inspect(std::string(64, 'f')));
    assert(floor.advance_after_external_signature_review(
               std::string(64, 'f'), 1, 2, sha256_hex("foreign"))
        == FloorCode::stale_or_conflicting_floor);
    assert(floor.advance_after_external_signature_review(
               trusted, 1, 1, sha256_hex("not higher"))
        == FloorCode::invalid);
    assert(floor.advance_after_external_signature_review(
               trusted, 2, 3, sha256_hex("stale"))
        == FloorCode::stale_or_conflicting_floor);
    assert(floor.advance_after_external_signature_review(
               trusted, 1, 2, "not-sha256")
        == FloorCode::invalid);
    assert(floor.advance_after_external_signature_review(
               trusted, 1, UINT64_MAX, sha256_hex("overflow"))
        == FloorCode::invalid);
    assert(floor.advance_after_external_signature_review(
               trusted, 1, 0, sha256_hex("decrease"))
        == FloorCode::invalid);
    assert(floor.inspect(trusted)->minimum_generation == 1);
}

void test_two_separate_writers_only_one_advance()
{
    Fixture f;
    Temp t("race");
    const auto trusted = root_fp(f.root);
    {
        FloorLedger initial(t.db.string(), FloorMode::create_new_isolated,
                            trusted);
        assert(initial.inspect(trusted)->minimum_generation == 1);
    }

    std::atomic<int> committed {0}, conflicts {0};
    auto worker = [&](int i) {
        try {
            FloorLedger writer(t.db.string(), FloorMode::existing_writer);
            const auto code = writer.advance_after_external_signature_review(
                trusted, 1, 2, sha256_hex(std::to_string(i)));
            if (code == FloorCode::advanced_no_authority) ++committed;
            else {
                assert(code == FloorCode::stale_or_conflicting_floor
                    || code == FloorCode::unavailable_or_commit_uncertain);
                ++conflicts;
            }
        } catch (...) {
            // Opening during the other writer's SQLite journal is safely
            // rejected rather than bypassing the exclusive transaction.
            ++conflicts;
        }
    };
    std::thread first(worker, 1), second(worker, 2);
    first.join();
    second.join();
    assert(committed == 1 && conflicts == 1);
    FloorLedger readback(t.db.string(), FloorMode::existing_read_only);
    assert(readback.inspect(trusted)->minimum_generation == 2);
}

void test_missing_unsafe_and_untrusted_storage_fail_closed()
{
    Fixture f;
    Temp t("unsafe");
    const auto root = root_fp(f.root);
    bool failed = false;
    try { FloorLedger absent(t.db.string(), FloorMode::existing_writer); }
    catch (...) { failed = true; }
    assert(failed && !std::filesystem::exists(t.db));

    {
        FloorLedger initial(t.db.string(), FloorMode::create_new_isolated, root);
        assert(initial.inspect(root));
    }
    failed = false;
    try { FloorLedger exists(t.db.string(), FloorMode::create_new_isolated, root); }
    catch (...) { failed = true; }
    assert(failed);

    assert(chmod(t.db.string().c_str(), 0644) == 0);
    failed = false;
    try { FloorLedger unsafe(t.db.string(), FloorMode::existing_read_only); }
    catch (...) { failed = true; }
    assert(failed);
    assert(chmod(t.db.string().c_str(), 0600) == 0);

    const auto sym = t.dir / "symlink.sqlite";
    std::filesystem::create_symlink(t.db, sym);
    failed = false;
    try { FloorLedger link(sym.string(), FloorMode::existing_read_only); }
    catch (...) { failed = true; }
    assert(failed);
    const auto hard = t.dir / "hardlink.sqlite";
    std::filesystem::create_hard_link(t.db, hard);
    failed = false;
    try { FloorLedger hard_ref(t.db.string(), FloorMode::existing_read_only); }
    catch (...) { failed = true; }
    assert(failed);
    std::filesystem::remove(hard);

    assert(chmod(t.dir.string().c_str(), 0755) == 0);
    failed = false;
    try { FloorLedger loose_dir(t.db.string(), FloorMode::existing_read_only); }
    catch (...) { failed = true; }
    assert(failed);
    assert(chmod(t.dir.string().c_str(), 0700) == 0);

    {
        std::ofstream companion(t.db.string() + "-wal");
        assert(companion.good());
    }
    failed = false;
    try { FloorLedger wal(t.db.string(), FloorMode::existing_writer); }
    catch (...) { failed = true; }
    assert(failed);
    std::filesystem::remove(t.db.string() + "-wal");

    FloorLedger good(t.db.string(), FloorMode::existing_read_only);
    assert(good.inspect(root)->minimum_generation == 1);
}

} // namespace

int main()
{
    test_floor_survives_restart_and_blocks_old_root_signed_active();
    test_malformed_foreign_root_and_non_monotonic_updates();
    test_two_separate_writers_only_one_advance();
    test_missing_unsafe_and_untrusted_storage_fail_closed();
    return 0;
}
