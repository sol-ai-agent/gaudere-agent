#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
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

Code check(const Review& r, const Proposal& p, const Scope& s,
           const Challenge& c, const Signed& proof, const SyntheticKey& key,
           std::int64_t now = 5000, const std::string& pinned = "bertrand")
{
    return inspect_local_goose_dialogue_responder_retry_exact_message_signature(
        r, p, s, c, proof, key.pub, pinned, now);
}

void test_exact_message_signature()
{
    const auto s = scope(), p = proposal(s), c = challenge(p, s);
    SyntheticKey key(0x42);
    const std::string message = u8"Réponse précise.\nÀ bientôt.";
    const auto r = review(p, s, c, message);
    const auto proof = sign(r, p, s, c, key);
    assert(r.message_sha256 == sha256_hex(message));
    assert(r.message_kind == local_goose_dialogue_responder_message_intervention);
    assert(r.retry_generation == 1 && r.completion_sequence == 4);
    assert(r.expected_head_revision == 3);
    assert(check(r, p, s, c, proof, key)
        == Code::cryptographically_consistent_no_authority);
    // Stateless validity is NOT replay protection. The ledger is distinct.
    assert(check(r, p, s, c, proof, key)
        == Code::cryptographically_consistent_no_authority);
    assert(check(r, p, s, c, proof, key, 3999) == Code::expired);
    assert(check(r, p, s, c, proof, key, 14000) == Code::expired);
    assert(check(r, p, s, c, proof, key, 5000, "other")
        == Code::untrusted_key_or_actor);
    SyntheticKey foreign(0x24);
    assert(check(r, p, s, c, proof, foreign)
        == Code::untrusted_key_or_actor);
}

void test_message_tamper_and_normalization()
{
    const auto s = scope(), p = proposal(s), c = challenge(p, s);
    SyntheticKey key(0x42);
    const auto r = review(p, s, c, u8"Café");
    const auto proof = sign(r, p, s, c, key);
    assert(check(r, p, s, c, proof, key)
        == Code::cryptographically_consistent_no_authority);

    // Visually similar UTF-8 representations are distinct signed bytes:
    // normalization must be a separate, human-reviewed contract.
    const auto decomposed = review(p, s, c, u8"Cafe\u0301");
    assert(decomposed.review_id != r.review_id);
    assert(check(decomposed, p, s, c, proof, key)
        == Code::untrusted_key_or_actor);

    auto modified = r;
    modified.message_text += "!";
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);
    modified = r;
    modified.message_sha256 = std::string(64, '0');
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);
    modified = r;
    modified.expected_head_revision = 4;
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);
    modified = r;
    modified.message_kind = local_goose_dialogue_responder_message_dialogue;
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);
    modified = r;
    modified.responder_speaker_id = "other";
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);
    modified = r;
    modified.review_id = id(
        "cognition.local-goose-dialogue.responder-retry-exact-message-review.v1:", '0');
    assert(check(modified, p, s, c, proof, key)
        == Code::invalid_challenge_or_message);

    auto changed = proof;
    changed.signature_hex[0] = changed.signature_hex[0] == '0' ? '1' : '0';
    assert(check(r, p, s, c, changed, key) == Code::signature_invalid);
    changed = proof;
    changed.signature_hex.pop_back();
    assert(check(r, p, s, c, changed, key) == Code::signature_invalid);
    changed = proof;
    changed.operator_id = "other";
    assert(check(r, p, s, c, changed, key)
        == Code::untrusted_key_or_actor);
    changed = proof;
    changed.decision = "approve_retry_generation_1";
    assert(check(r, p, s, c, changed, key)
        == Code::untrusted_key_or_actor);
}

void test_proposal_challenge_and_old_signature_isolation()
{
    const auto s = scope(), p = proposal(s), c = challenge(p, s);
    SyntheticKey key(0x42);
    const auto r = review(p, s, c, "One exact intervention.");
    const auto proof = sign(r, p, s, c, key);
    auto changed_c = c;
    changed_c.nonce_hex[0] = 'b';
    assert(check(r, p, s, changed_c, proof, key)
        == Code::invalid_challenge_or_message);
    changed_c = c;
    changed_c.expires_at_ms += 1;
    assert(check(r, p, s, changed_c, proof, key)
        == Code::invalid_challenge_or_message);
    auto changed_p = p;
    changed_p.max_system_turns = 2;
    assert(check(r, changed_p, s, c, proof, key)
        == Code::invalid_scope_or_proposal);
    auto changed_s = s;
    changed_s.completion_sequence = 5;
    assert(check(r, p, changed_s, c, proof, key)
        == Code::invalid_scope_or_proposal);

    // The v1 proposal-only signature cannot be replayed as exact-message v2.
    const auto old = local_goose_dialogue_responder_retry_operator_signing_payload(
        c, "bertrand", "approve_retry_generation_1");
    assert(old);
    auto old_signature = proof;
    old_signature.signature_hex = sign_payload(key, *old);
    assert(check(r, p, s, c, old_signature, key)
        == Code::signature_invalid);
}

void test_malformed_text_and_boundaries()
{
    const auto s = scope(), p = proposal(s), c = challenge(p, s);
    const auto make = [&](const std::string& text) {
        return make_local_goose_dialogue_responder_retry_exact_message_review(
            p, s, c, text);
    };
    assert(!make(""));
    assert(!make(std::string(4097, 'A')));
    assert(make(std::string(4096, 'A')));
    assert(!make("hello\rworld"));
    assert(!make("hello\tworld"));
    assert(!make(std::string("NUL\\0", 5)));
    assert(!make(std::string("\xc0\xaf", 2))); // overlong
    assert(!make(std::string("\xed\xa0\x80", 3))); // UTF-8 surrogate
    assert(!make(std::string("\xf4\x90\x80\x80", 4))); // > U+10ffff
    assert(!make(std::string("\xe2\x80\xae", 3))); // bidi override
    assert(!make(std::string("\xe2\x80\x8b", 3))); // zero-width space
    assert(!make(std::string("\xe2\x82", 2))); // truncated
    assert(!make(std::string("\xff", 1)));
    assert(make(u8"Une phrase en français.\nSeconde ligne."));
}

} // namespace

int main()
{
    test_exact_message_signature();
    test_message_tamper_and_normalization();
    test_proposal_challenge_and_old_signature_isolation();
    test_malformed_text_and_boundaries();
    return 0;
}
