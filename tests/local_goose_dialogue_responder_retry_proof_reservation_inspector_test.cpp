#include "LocalGooseDialogueResponderRetryProofReservationInspector.hpp"
#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"
#include "LocalGooseDialogueResponder.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <sys/stat.h>
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


using InspectCode = LocalGooseDialogueResponderRetryProofReservationCode;
using Ledger = LocalGooseDialogueResponderRetryChallengeLedger;
using OpenMode = LocalGooseDialogueResponderRetryChallengeLedgerOpenMode;
using LedgerCode = LocalGooseDialogueResponderRetryChallengeLedgerCode;
using State = LocalGooseDialogueResponderRetryChallengeState;

struct Temp {
    explicit Temp(const std::string& tag)
    {
        static unsigned serial = 0;
        path = std::filesystem::temp_directory_path()
            / ("gaudere-retry-proof-ledger-" + tag + "-"
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

std::string snapshot(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file.good());
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

struct Fixture {
    Scope s = scope();
    Proposal p = proposal(s);
    Challenge c = challenge(p, s);
    Review r = review(p, s, c, u8"Réponse exacte.\\nÀ bientôt.");
    SyntheticKey key {0x42};
    Signed proof = sign(r, p, s, c, key);
};

LocalGooseDialogueResponderRetryChallengeReservation reservation(
    const Fixture& f, const std::string& request = "reserve-request-1",
    const std::string& nonce_override = "")
{
    const auto value =
        make_local_goose_dialogue_responder_retry_challenge_reservation(
            request, f.c.challenge_id, f.p.proposal_id, f.s.scope_id,
            f.s.lineage_id,
            nonce_override.empty()
                ? local_goose_dialogue_responder_retry_nonce_fingerprint(
                    f.c.nonce_hex)
                : nonce_override,
            f.c.issued_at_ms, f.c.expires_at_ms);
    assert(value);
    return *value;
}

void stage(const std::filesystem::path& path,
           const LocalGooseDialogueResponderRetryChallengeReservation& r)
{
    Ledger ledger(path.string(), OpenMode::create_new_isolated);
    assert(ledger.reserve(r) == LedgerCode::accepted);
}

InspectCode inspect(const Fixture& f, const std::filesystem::path& path,
                    const std::string& request = "reserve-request-1",
                    const std::int64_t now = 5000,
                    const Signed* signed_override = nullptr,
                    const Review* review_override = nullptr)
{
    return inspect_local_goose_dialogue_responder_retry_proof_reservation(
        path.string(), request,
        review_override ? *review_override : f.r,
        f.p, f.s, f.c,
        signed_override ? *signed_override : f.proof,
        f.key.pub, "bertrand", now);
}

void test_consistent_reservation_is_only_an_observation()
{
    Fixture f;
    Temp t("observe");
    const auto r = reservation(f);
    stage(t.path, r);
    const auto before = snapshot(t.path);
    assert(inspect(f, t.path) == InspectCode::consistent_reserved_no_authority);
    // Stateless, repeatable observation: NOT a grant or one-time consumption.
    assert(inspect(f, t.path) == InspectCode::consistent_reserved_no_authority);
    assert(snapshot(t.path) == before);
    assert(!std::filesystem::exists(t.path.string() + "-wal"));
    assert(!std::filesystem::exists(t.path.string() + "-journal"));
    assert(!std::filesystem::exists(t.path.string() + "-shm"));
    {
        Ledger readonly(t.path.string(), OpenMode::existing_read_only);
        const auto row = readonly.find_challenge(f.c.challenge_id);
        assert(row && row->state == State::reserved);
        assert(!row->evidence_sha256);
    }
}

void test_signature_and_expected_request_fail_closed()
{
    Fixture f;
    Temp t("proof");
    stage(t.path, reservation(f));
    const auto before = snapshot(t.path);
    auto bad_proof = f.proof;
    bad_proof.signature_hex[0] =
        bad_proof.signature_hex[0] == '0' ? '1' : '0';
    assert(inspect(f, t.path, "reserve-request-1", 5000, &bad_proof)
        == InspectCode::invalid_exact_message_signature);
    auto changed_review = f.r;
    changed_review.message_text += " altered";
    assert(inspect(f, t.path, "reserve-request-1", 5000, nullptr,
                   &changed_review)
        == InspectCode::invalid_exact_message_signature);
    assert(inspect(f, t.path, "reserve-request-1", 14000)
        == InspectCode::invalid_exact_message_signature);
    assert(inspect(f, t.path, "bad request")
        == InspectCode::invalid_expected_reservation);
    assert(inspect(f, t.path, "other-valid-request")
        == InspectCode::reservation_mismatch);
    assert(snapshot(t.path) == before);
}

void test_nonce_hash_and_immutable_scope_binding()
{
    Fixture f;
    assert(local_goose_dialogue_responder_retry_nonce_fingerprint(
        std::string(64, '0')).empty());
    assert(local_goose_dialogue_responder_retry_nonce_fingerprint(
        std::string(64, 'G')).empty());
    assert(local_goose_dialogue_responder_retry_nonce_fingerprint("a").empty());
    const auto good_nonce =
        local_goose_dialogue_responder_retry_nonce_fingerprint(f.c.nonce_hex);
    assert(good_nonce.size() == 64);
    assert(good_nonce
        != local_goose_dialogue_responder_retry_nonce_fingerprint(
            std::string(64, 'b')));
    {
        Temp t("nonce");
        stage(t.path, reservation(f, "reserve-request-1", std::string(64, 'b')));
        assert(inspect(f, t.path) == InspectCode::reservation_mismatch);
    }
    {
        Temp t("scope");
        auto r = reservation(f);
        // All fields remain syntactically valid, but the reservation is not
        // the same exact scope and its canonical identity changes.
        const auto changed =
            make_local_goose_dialogue_responder_retry_challenge_reservation(
                r.request_id, r.challenge_id, r.proposal_id,
                id("cognition.local-goose-dialogue.responder-retry-scope.v1:",
                   'f'),
                r.lineage_id, r.nonce_sha256, r.issued_at_ms, r.expires_at_ms);
        assert(changed);
        stage(t.path, *changed);
        assert(inspect(f, t.path) == InspectCode::reservation_mismatch);
    }
    {
        Temp t("timestamps");
        auto r = reservation(f);
        const auto changed =
            make_local_goose_dialogue_responder_retry_challenge_reservation(
                r.request_id, r.challenge_id, r.proposal_id, r.scope_id,
                r.lineage_id, r.nonce_sha256,
                r.issued_at_ms, r.expires_at_ms + 1);
        assert(changed);
        stage(t.path, *changed);
        assert(inspect(f, t.path) == InspectCode::reservation_mismatch);
    }
}

void test_absorbing_consumption_and_uncertainty()
{
    Fixture f;
    for (const bool uncertain : {false, true}) {
        Temp t(uncertain ? "uncertain" : "burned");
        const auto r = reservation(f);
        stage(t.path, r);
        {
            Ledger ledger(t.path.string(), OpenMode::existing_writer);
            assert((uncertain
                ? ledger.mark_uncertain(r.challenge_id, r.request_id)
                : ledger.burn(r.challenge_id, r.request_id,
                              std::string(64, 'f'))) == LedgerCode::accepted);
        }
        const auto before = snapshot(t.path);
        assert(inspect(f, t.path)
            == InspectCode::reservation_already_used_or_uncertain);
        assert(snapshot(t.path) == before);
    }
}

void test_missing_and_unsafe_ledger_never_created()
{
    Fixture f;
    Temp absent("missing");
    assert(inspect(f, absent.path)
        == InspectCode::ledger_unavailable_or_unsafe);
    assert(!std::filesystem::exists(absent.path));

    Temp t("unsafe");
    stage(t.path, reservation(f));
    assert(chmod(t.path.string().c_str(), 0644) == 0);
    assert(inspect(f, t.path)
        == InspectCode::ledger_unavailable_or_unsafe);
    assert(chmod(t.path.string().c_str(), 0600) == 0);

    Temp link("symlink");
    std::filesystem::create_symlink(t.path, link.path);
    assert(inspect(f, link.path)
        == InspectCode::ledger_unavailable_or_unsafe);

    std::ofstream(t.path.string() + "-wal") << "untrusted companion";
    assert(inspect(f, t.path)
        == InspectCode::ledger_unavailable_or_unsafe);
    std::filesystem::remove(t.path.string() + "-wal");
    assert(inspect(f, t.path) == InspectCode::consistent_reserved_no_authority);
}

} // namespace

int main()
{
    test_consistent_reservation_is_only_an_observation();
    test_signature_and_expected_request_fail_closed();
    test_nonce_hash_and_immutable_scope_binding();
    test_absorbing_consumption_and_uncertainty();
    test_missing_and_unsafe_ledger_never_created();
    return 0;
}
