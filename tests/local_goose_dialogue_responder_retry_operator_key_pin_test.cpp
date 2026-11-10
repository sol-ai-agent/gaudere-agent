#include "LocalGooseDialogueResponderRetryOperatorKeyPin.hpp"
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

struct Fixture {
    Scope s = scope();
    Proposal p = proposal(s);
    Challenge c = challenge(p, s);
    Review r = review(p, s, c, u8"Réponse exacte.\nÀ bientôt.");
    SyntheticKey key {0x42};
    Signed proof = sign(r, p, s, c, key);
};

struct Temp {
    explicit Temp(const std::string& tag)
    {
        static unsigned serial = 0;
        path = std::filesystem::temp_directory_path()
            / ("gaudere-operator-pin-" + tag + "-"
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

std::string to_hex(const unsigned char* raw, const std::size_t n)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < n; ++i) {
        result.push_back(digits[(raw[i] >> 4u) & 15u]);
        result.push_back(digits[raw[i] & 15u]);
    }
    return result;
}

std::string pin_content(const SyntheticKey& key)
{
    return std::string(local_goose_dialogue_responder_retry_key_pin_header)
        + to_hex(key.pub.data(), key.pub.size()) + "\n";
}

void write_pin(const std::filesystem::path& path, const std::string& data)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out.good());
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    assert(out.good());
    assert(chmod(path.string().c_str(), 0400) == 0);
}

PinCode inspect_pin(
    const Fixture& f, const std::filesystem::path& path,
    const std::string& trusted_fingerprint,
    const Signed* overridden = nullptr,
    const Review* override_review = nullptr,
    std::int64_t now_ms = 5000)
{
    return inspect_local_goose_dialogue_responder_retry_pinned_signature(
        path.string(), trusted_fingerprint,
        override_review ? *override_review : f.r,
        f.p, f.s, f.c, overridden ? *overridden : f.proof, now_ms);
}

void test_pin_and_exact_signature()
{
    Fixture f;
    Temp t("valid");
    write_pin(t.path, pin_content(f.key));
    const auto trusted = f.proof.key_fingerprint;
    assert(trusted.size() == 64);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::cryptographically_consistent_no_authority);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::cryptographically_consistent_no_authority); // NO one-time use
    assert(inspect_pin(f, t.path, std::string(64, 'f'))
        == PinCode::trusted_fingerprint_invalid_or_mismatch);
    assert(inspect_pin(f, t.path, "attacker-supplied-trust")
        == PinCode::trusted_fingerprint_invalid_or_mismatch);

    auto bad_proof = f.proof;
    bad_proof.signature_hex[0] =
        bad_proof.signature_hex[0] == '0' ? '1' : '0';
    assert(inspect_pin(f, t.path, trusted, &bad_proof)
        == PinCode::signed_message_invalid_or_expired);
    auto changed_review = f.r;
    changed_review.message_text += " (altered)";
    assert(inspect_pin(f, t.path, trusted, nullptr, &changed_review)
        == PinCode::signed_message_invalid_or_expired);
    assert(inspect_pin(f, t.path, trusted, nullptr, nullptr, 14000)
        == PinCode::signed_message_invalid_or_expired);
}

void test_unauthorized_key_cannot_replace_independent_trust()
{
    Fixture f;
    Temp t("foreign");
    SyntheticKey foreign(0x24);
    write_pin(t.path, pin_content(foreign));
    // A writable candidate pin cannot replace the independently trusted
    // fingerprint, even when the attacker has their own valid key pair.
    assert(inspect_pin(f, t.path, f.proof.key_fingerprint)
        == PinCode::trusted_fingerprint_invalid_or_mismatch);
    assert(inspect_pin(f, t.path, f.proof.key_fingerprint)
        != PinCode::cryptographically_consistent_no_authority);
}

void test_file_safety()
{
    Fixture f;
    const auto trusted = f.proof.key_fingerprint;
    Temp absent("missing");
    assert(inspect_pin(f, absent.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);
    assert(!std::filesystem::exists(absent.path));
    assert(inspect_pin(f, std::filesystem::path("relative.pin"), trusted)
        == PinCode::key_pin_unavailable_or_unsafe);

    Temp t("mode");
    write_pin(t.path, pin_content(f.key));
    assert(chmod(t.path.string().c_str(), 0644) == 0);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);
    assert(chmod(t.path.string().c_str(), 0600) == 0);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);
    assert(chmod(t.path.string().c_str(), 0400) == 0);

    Temp symlink("symlink");
    std::filesystem::create_symlink(t.path, symlink.path);
    assert(inspect_pin(f, symlink.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);

    Temp hardlink("hardlink");
    std::filesystem::create_hard_link(t.path, hardlink.path);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);
    assert(inspect_pin(f, hardlink.path, trusted)
        == PinCode::key_pin_unavailable_or_unsafe);
    std::filesystem::remove(hardlink.path);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::cryptographically_consistent_no_authority);
}

void test_strict_canonical_format_and_bounds()
{
    Fixture f;
    const auto trusted = f.proof.key_fingerprint;
    Temp t("malformed");
    const std::string good = pin_content(f.key);
    const auto reject = [&](const std::string& content) {
        assert(chmod(t.path.string().c_str(), 0600) == 0);
        write_pin(t.path, content);
        assert(inspect_pin(f, t.path, trusted)
            == PinCode::key_pin_unavailable_or_unsafe);
    };
    write_pin(t.path, good);
    reject(good + "\n");
    reject(good.substr(0, good.size()-1));
    reject(std::string("attacker-key\n") + good.substr(13));
    reject(std::string(local_goose_dialogue_responder_retry_key_pin_header)
        + std::string(64,'0') + "\n");
    std::string uppercase = good;
    uppercase[uppercase.size() - 2] = 'F';
    reject(uppercase);
    assert(chmod(t.path.string().c_str(), 0600) == 0);
    write_pin(t.path, good);
    assert(inspect_pin(f, t.path, trusted)
        == PinCode::cryptographically_consistent_no_authority);
}

} // namespace

int main()
{
    test_pin_and_exact_signature();
    test_unauthorized_key_cannot_replace_independent_trust();
    test_file_safety();
    test_strict_canonical_format_and_bounds();
    return 0;
}
