#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"
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
        static unsigned serial = 0;
        path = std::filesystem::temp_directory_path()
            / ("gaudere-retry-trust-" + tag + "-"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())
                + "-" + std::to_string(++serial) + ".pin");
    }
    ~Temp()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    std::filesystem::path path;
};

std::string hex_key(const SyntheticKey& key)
{
    return hex_signature(key.pub.data(), key.pub.size());
}

void write_pin(const std::filesystem::path& path, const SyntheticKey& key)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.good());
    const auto value =
        std::string(local_goose_dialogue_responder_retry_key_pin_header)
        + hex_key(key) + "\n";
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
    out.close();
    assert(out.good());
    assert(chmod(path.string().c_str(), 0400) == 0);
}

std::string root_fp(const SyntheticKey& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.pub.data()),
                          key.pub.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

std::string snapshot(Fixture& f,
                     const std::string& status = "active",
                     const std::string& fingerprint = "",
                     const std::string& generation = "7",
                     const std::string& from = "1000",
                     const std::string& until = "30000")
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_trust_snapshot_header)
        + "key_fingerprint="
        + (fingerprint.empty() ? f.proof.key_fingerprint : fingerprint) + "\n"
        + "status=" + status + "\n"
        + "generation=" + generation + "\n"
        + "not_before_ms=" + from + "\n"
        + "not_after_ms=" + until + "\n";
    return body + "signature_hex="
        + sign_payload(f.root,
            "gaudere-retry-operator-trust-snapshot-signature.v1|" + body)
        + "\n";
}

TrustCode check(Fixture& f, const std::filesystem::path& path,
                const std::string& snap,
                const std::uint64_t minimum_generation = 7,
                const std::int64_t now_ms = 5000,
                const std::string& root_fingerprint_override = "",
                const std::array<unsigned char, 32>* root_override = nullptr,
                const Signed* proof_override = nullptr)
{
    return inspect_local_goose_dialogue_responder_retry_operator_trust_snapshot(
        snap, root_override ? *root_override : f.root.pub,
        root_fingerprint_override.empty() ? root_fp(f.root)
                                          : root_fingerprint_override,
        minimum_generation, path.string(), f.r, f.p, f.s, f.c,
        proof_override ? *proof_override : f.proof, now_ms);
}

void test_root_signed_active_snapshot_and_exact_message()
{
    Fixture f;
    Temp path("active");
    write_pin(path.path, f.key);
    const auto valid = snapshot(f);
    assert(check(f, path.path, valid)
        == TrustCode::cryptographically_consistent_no_authority);
    assert(check(f, path.path, valid)
        == TrustCode::cryptographically_consistent_no_authority);
    assert(check(f, path.path, valid, 0)
        == TrustCode::invalid_root_or_independent_trust);
    assert(check(f, path.path, valid, 7, 5000, std::string(64, 'f'))
        == TrustCode::invalid_root_or_independent_trust);
    SyntheticKey foreign_root(0x17);
    assert(check(f, path.path, valid, 7, 5000, "", &foreign_root.pub)
        == TrustCode::invalid_root_or_independent_trust);

    auto forged = f.proof;
    forged.signature_hex[0] =
        forged.signature_hex[0] == '0' ? '1' : '0';
    assert(check(f, path.path, valid, 7, 5000, "", nullptr, &forged)
        == TrustCode::operator_pin_or_exact_message_invalid);
    assert(check(f, path.path, valid, 7, 14000)
        == TrustCode::operator_pin_or_exact_message_invalid);
}

void test_revoke_and_floor_against_old_signed_active()
{
    Fixture f;
    Temp path("revoked");
    write_pin(path.path, f.key);
    const auto old_active = snapshot(f, "active", "", "7");
    const auto newer_revoked = snapshot(f, "revoked", "", "8");
    assert(check(f, path.path, old_active, 7)
        == TrustCode::cryptographically_consistent_no_authority);
    assert(check(f, path.path, newer_revoked, 8)
        == TrustCode::revoked_or_stale_or_outside_validity);
    assert(check(f, path.path, old_active, 8)
        == TrustCode::revoked_or_stale_or_outside_validity);
    assert(check(f, path.path, old_active, 9)
        == TrustCode::revoked_or_stale_or_outside_validity);
    assert(check(f, path.path, snapshot(f, "active", "", "9"), 9)
        == TrustCode::cryptographically_consistent_no_authority);
    assert(check(f, path.path, old_active, 7, 500)
        == TrustCode::revoked_or_stale_or_outside_validity);
    assert(check(f, path.path, old_active, 7, 30000)
        == TrustCode::revoked_or_stale_or_outside_validity);
}

void test_root_signature_and_snapshot_tampering()
{
    Fixture f;
    Temp path("tampered");
    write_pin(path.path, f.key);
    const auto good = snapshot(f);
    const std::size_t pos = good.find("status=active");
    assert(pos != std::string::npos);
    auto changed = good;
    changed.replace(pos, 13, "status=revoked");
    assert(check(f, path.path, changed)
        == TrustCode::invalid_root_signature);
    changed = good;
    const std::size_t sig = changed.find("signature_hex=");
    assert(sig != std::string::npos);
    changed[sig + 14] = changed[sig + 14] == '0' ? '1' : '0';
    assert(check(f, path.path, changed)
        == TrustCode::invalid_root_signature);

    const auto duplicate = good.substr(0, pos) + "status=active\n"
        + good.substr(pos);
    assert(check(f, path.path, duplicate)
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, good + "\n")
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, good.substr(0, good.size() - 1))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "enabled"))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "active", "", "07"))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "active", "", "0"))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "active", "", "7", "200", "100"))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "active", "", "7", "1000", "9999999999999"))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, snapshot(f, "active", std::string(64,'G')))
        == TrustCode::invalid_snapshot_format);
    assert(check(f, path.path, "not a snapshot")
        == TrustCode::invalid_snapshot_format);
}

void test_key_rotation_requires_signed_policy_and_matching_local_pin()
{
    Fixture f;
    Temp path("rotation");
    write_pin(path.path, f.key);
    SyntheticKey rotated(0x71);
    const Signed rotated_proof = sign(f.r, f.p, f.s, f.c, rotated);
    const auto snap_new = snapshot(f, "active", rotated_proof.key_fingerprint, "8");
    const auto snap_old = snapshot(f, "active", "", "7");

    assert(check(f, path.path, snap_new, 8, 5000, "", nullptr,
                 &rotated_proof)
        == TrustCode::operator_pin_or_exact_message_invalid);
    assert(chmod(path.path.string().c_str(), 0600) == 0);
    write_pin(path.path, rotated);
    assert(check(f, path.path, snap_new, 8, 5000, "", nullptr,
                 &rotated_proof)
        == TrustCode::cryptographically_consistent_no_authority);
    assert(check(f, path.path, snap_old, 8)
        == TrustCode::revoked_or_stale_or_outside_validity);
    assert(check(f, path.path, snap_new, 8)
        == TrustCode::operator_pin_or_exact_message_invalid);
}

void test_unsafe_operator_pin_blocks_root_signed_active()
{
    Fixture f;
    Temp path("unsafe");
    write_pin(path.path, f.key);
    const auto active = snapshot(f);
    assert(chmod(path.path.string().c_str(), 0644) == 0);
    assert(check(f, path.path, active)
        == TrustCode::operator_pin_or_exact_message_invalid);
    assert(chmod(path.path.string().c_str(), 0400) == 0);
    Temp link("symlink");
    std::filesystem::create_symlink(path.path, link.path);
    assert(check(f, link.path, active)
        == TrustCode::operator_pin_or_exact_message_invalid);
}

} // namespace

int main()
{
    test_root_signed_active_snapshot_and_exact_message();
    test_revoke_and_floor_against_old_signed_active();
    test_root_signature_and_snapshot_tampering();
    test_key_rotation_requires_signed_policy_and_matching_local_pin();
    test_unsafe_operator_pin_blocks_root_signed_active();
    return 0;
}
