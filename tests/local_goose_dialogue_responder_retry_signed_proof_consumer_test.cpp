#include "LocalGooseDialogueResponderRetrySignedProofConsumer.hpp"
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


using ConsumeCode = LocalGooseDialogueResponderRetrySignedProofConsumeCode;

ConsumeCode consume(
    const Fixture& f, const std::filesystem::path& path,
    const std::string& request = "reserve-request-1",
    const std::int64_t now = 5000,
    const Signed* signed_override = nullptr,
    const Review* review_override = nullptr)
{
    return consume_local_goose_dialogue_responder_retry_signed_proof_once(
        path.string(), request, review_override ? *review_override : f.r,
        f.p, f.s, f.c, signed_override ? *signed_override : f.proof,
        f.key.pub, "bertrand", now);
}

void test_exact_signed_proof_burned_once_across_restart()
{
    Fixture f;
    Temp t("burn");
    stage(t.path, reservation(f));
    const auto before = snapshot(t.path);
    assert(inspect(f, t.path) == InspectCode::consistent_reserved_no_authority);
    assert(snapshot(t.path) == before);
    assert(consume(f, t.path) == ConsumeCode::burned_no_authority);
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);
    {
        Ledger ledger(t.path.string(), OpenMode::existing_read_only);
        const auto row = ledger.find_challenge(f.c.challenge_id);
        assert(row && row->state == State::burned);
        assert(row->evidence_sha256 && row->evidence_sha256->size() == 64);
    }
    assert(inspect(f, t.path)
        == InspectCode::reservation_already_used_or_uncertain);
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);
}

void test_invalid_signature_message_and_expiry_do_not_burn()
{
    Fixture f;
    Temp t("reject");
    stage(t.path, reservation(f));
    const auto before = snapshot(t.path);
    auto wrong_proof = f.proof;
    wrong_proof.signature_hex[0] =
        wrong_proof.signature_hex[0] == '0' ? '1' : '0';
    assert(consume(f, t.path, "reserve-request-1", 5000, &wrong_proof)
        == ConsumeCode::invalid_signed_message_or_expired);
    auto changed = f.r;
    changed.message_text += " changed";
    assert(consume(f, t.path, "reserve-request-1", 5000, nullptr, &changed)
        == ConsumeCode::invalid_signed_message_or_expired);
    assert(consume(f, t.path, "reserve-request-1", 14000)
        == ConsumeCode::invalid_signed_message_or_expired);
    assert(consume(f, t.path, "not a request")
        == ConsumeCode::invalid_expected_reservation);
    assert(consume(f, t.path, "different-valid-request")
        == ConsumeCode::already_burned_uncertain_or_mismatch);
    assert(snapshot(t.path) == before);
    assert(inspect(f, t.path) == InspectCode::consistent_reserved_no_authority);
}

void test_exact_reservation_verified_inside_burn()
{
    Fixture f;
    Temp t("mismatch");
    // Attacker's otherwise well-formed reservation contains a different
    // nonce fingerprint than the exact signed operator challenge.
    auto wrong = reservation(f, "reserve-request-1", std::string(64, 'b'));
    stage(t.path, wrong);
    const auto before = snapshot(t.path);
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);
    assert(snapshot(t.path) == before);
    {
        Ledger writer(t.path.string(), OpenMode::existing_writer);
        auto altered_expected = reservation(f);
        altered_expected.scope_id = id(
            "cognition.local-goose-dialogue.responder-retry-scope.v1:", 'f');
        assert(writer.burn_if_exact(altered_expected, std::string(64, 'a'))
            == LedgerCode::invalid);
        assert(writer.burn_if_exact(wrong, "bad-sha")
            == LedgerCode::invalid);
        assert(writer.burn_if_exact(wrong, std::string(64, 'a'))
            == LedgerCode::accepted);
        assert(writer.burn_if_exact(wrong, std::string(64, 'a'))
            == LedgerCode::conflict);
    }
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);
}

void test_uncertainty_missing_and_permissions_fail_closed()
{
    Fixture f;
    Temp missing("missing");
    assert(consume(f, missing.path)
        == ConsumeCode::ledger_unavailable_or_commit_uncertain);
    assert(!std::filesystem::exists(missing.path));

    Temp t("uncertain");
    const auto r = reservation(f);
    stage(t.path, r);
    {
        Ledger l(t.path.string(), OpenMode::existing_writer);
        assert(l.mark_uncertain(r.challenge_id, r.request_id)
            == LedgerCode::accepted);
    }
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);

    Temp untrusted("mode");
    stage(untrusted.path, r);
    assert(chmod(untrusted.path.string().c_str(), 0644) == 0);
    assert(consume(f, untrusted.path)
        == ConsumeCode::ledger_unavailable_or_commit_uncertain);
    assert(chmod(untrusted.path.string().c_str(), 0600) == 0);
    assert(consume(f, untrusted.path) == ConsumeCode::burned_no_authority);

    Temp link("symlink");
    std::filesystem::create_symlink(untrusted.path, link.path);
    assert(consume(f, link.path) ==
        ConsumeCode::ledger_unavailable_or_commit_uncertain);
}

void test_competing_writers_cannot_both_burn()
{
    Fixture f;
    Temp t("race");
    stage(t.path, reservation(f));
    std::atomic<int> accepted {0};
    std::atomic<int> errors {0};
    auto writer = [&] {
        const auto code = consume(f, t.path);
        if (code == ConsumeCode::burned_no_authority) {
            ++accepted;
        } else {
            assert(code == ConsumeCode::already_burned_uncertain_or_mismatch
                || code == ConsumeCode::ledger_unavailable_or_commit_uncertain);
            ++errors;
        }
    };
    std::thread first(writer), second(writer);
    first.join();
    second.join();
    assert(accepted.load() == 1 && errors.load() == 1);
    Ledger ledger(t.path.string(), OpenMode::existing_read_only);
    const auto row = ledger.find_challenge(f.c.challenge_id);
    assert(row && row->state == State::burned);
    assert(row->evidence_sha256 && row->evidence_sha256->size() == 64);
    assert(consume(f, t.path) ==
        ConsumeCode::already_burned_uncertain_or_mismatch);
}

} // namespace

int main()
{
    test_exact_signed_proof_burned_once_across_restart();
    test_invalid_signature_message_and_expiry_do_not_burn();
    test_exact_reservation_verified_inside_burn();
    test_uncertainty_missing_and_permissions_fail_closed();
    test_competing_writers_cannot_both_burn();
    return 0;
}
