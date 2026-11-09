#include "LocalGooseDialogueResponderRetryOperatorProof.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

namespace {

using namespace gaudere_agent;
using Scope = LocalGooseDialogueResponderRetryScope;
using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Challenge = LocalGooseDialogueResponderRetryOperatorChallenge;
using Signed = LocalGooseDialogueResponderRetrySignedOperatorProof;
using Code = LocalGooseDialogueResponderRetryOperatorProofCode;

std::string hash_id(const std::string& prefix, char c)
{
    return prefix + std::string(64, c);
}

Scope scope()
{
    const auto lineage = make_local_goose_dialogue_responder_retry_lineage(
        "lineage-request-1", "system-responder-v1", "main", 4,
        hash_id("cognition.local-goose-dialogue.completion.v1:", 'a'),
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
    assert(valid_local_goose_dialogue_responder_retry_lineage(lineage));
    const auto value = make_local_goose_dialogue_responder_retry_scope(
        "scope-request-1", lineage, 2000);
    assert(value);
    return *value;
}

Proposal proposal(const Scope& s)
{
    auto value = make_local_goose_dialogue_responder_retry_grant_proposal(
        s, "proposal-request-1", "approval-ref-1",
        "One reviewed intervention only", 3000, 300000, 120000);
    assert(value);
    return *value;
}

Challenge challenge(const Proposal& p, const Scope& s)
{
    auto value = make_local_goose_dialogue_responder_retry_operator_challenge(
        p, s, std::string(64, 'a'), 4000, 10000);
    assert(value);
    return *value;
}

struct SyntheticKey {
    std::array<unsigned char, 32> public_key {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key {
        nullptr, EVP_PKEY_free
    };

    explicit SyntheticKey(const unsigned char seed_byte)
    {
        // Synthetic CI-only seed: never ship a real private key in the product.
        std::array<unsigned char, 32> seed {};
        seed.fill(seed_byte);
        private_key.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(private_key);
        std::size_t n = public_key.size();
        assert(EVP_PKEY_get_raw_public_key(
            private_key.get(), public_key.data(), &n) == 1);
        assert(n == public_key.size());
    }
};

std::string hex_signature(const unsigned char* data, std::size_t n)
{
    constexpr char hexdigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(hexdigits[(data[i] >> 4) & 15]);
        s.push_back(hexdigits[data[i] & 15]);
    }
    return s;
}

Signed sign(const Challenge& c, SyntheticKey& key)
{
    Signed out;
    out.challenge_id = c.challenge_id;
    out.operator_id = "bertrand";
    out.decision = "approve_retry_generation_1";
    const auto payload = local_goose_dialogue_responder_retry_operator_signing_payload(
        c, out.operator_id, out.decision);
    assert(payload);
    const std::string raw_key(
        reinterpret_cast<const char*>(key.public_key.data()),
        key.public_key.size());
    out.key_fingerprint = sha256_hex(
        "gaudere-retry-operator-ed25519-key-v1|" + raw_key);

    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(ctx);
    assert(EVP_DigestSignInit(
        ctx.get(), nullptr, nullptr, nullptr, key.private_key.get()) == 1);
    std::array<unsigned char, 64> signature {};
    std::size_t length = signature.size();
    assert(EVP_DigestSign(ctx.get(), signature.data(), &length,
        reinterpret_cast<const unsigned char*>(payload->data()),
        payload->size()) == 1);
    assert(length == signature.size());
    out.signature_hex = hex_signature(signature.data(), length);
    return out;
}

Code inspect(const Scope& s, const Proposal& p, const Challenge& c,
             const Signed& signed_proof,
             const std::array<unsigned char, 32>& public_key,
             std::int64_t now = 5000,
             const std::string& pinned_actor = "bertrand")
{
    return inspect_local_goose_dialogue_responder_retry_operator_signature(
        p, s, c, signed_proof, public_key, pinned_actor, now).code;
}

void test_exact_pinned_key_and_statement()
{
    const auto s = scope();
    const auto p = proposal(s);
    const auto c = challenge(p, s);
    SyntheticKey key(0x2a);
    auto signed_proof = sign(c, key);
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::cryptographically_consistent_no_authority);

    // Verification is deterministic but stateless. It does not consume an
    // approval or claim replay protection: future durable broker must do so.
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::cryptographically_consistent_no_authority);

    SyntheticKey other_key(0x4c);
    assert(inspect(s, p, c, signed_proof, other_key.public_key)
        == Code::untrusted_key_or_actor);
    assert(inspect(s, p, c, signed_proof, key.public_key, 5000, "other")
        == Code::untrusted_key_or_actor);
    assert(inspect(s, p, c, signed_proof, key.public_key, 3999)
        == Code::expired);
    assert(inspect(s, p, c, signed_proof, key.public_key, 14000)
        == Code::expired);

    signed_proof.operator_id = "other";
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::untrusted_key_or_actor);
    signed_proof = sign(c, key);
    signed_proof.decision = "approve_all";
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::untrusted_key_or_actor);
    signed_proof = sign(c, key);
    signed_proof.signature_hex[0] =
        signed_proof.signature_hex[0] == '0' ? '1' : '0';
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::signature_invalid);
    signed_proof = sign(c, key);
    signed_proof.signature_hex.pop_back();
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::signature_invalid);
    signed_proof = sign(c, key);
    signed_proof.key_fingerprint[0] = '0';
    if (signed_proof.key_fingerprint[0]
            == sign(c, key).key_fingerprint[0])
        signed_proof.key_fingerprint[0] = '1';
    assert(inspect(s, p, c, signed_proof, key.public_key)
        == Code::untrusted_key_or_actor);
}

void test_exact_scope_nonce_and_challenge_binding()
{
    const auto s = scope();
    const auto p = proposal(s);
    const auto c = challenge(p, s);
    SyntheticKey key(0x2a);
    const auto statement = sign(c, key);

    auto changed = c;
    changed.nonce_hex[0] = 'b';
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    changed = c;
    changed.expires_at_ms++;
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    changed = c;
    changed.approval_reference = "approval-other";
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    changed = c;
    changed.challenge_id = hash_id(
        "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:",
        '1');
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    // Oversized/untrusted challenge fields fail before unbounded hashing.
    changed = c;
    changed.proposal_id = std::string(1024 * 1024, 'x');
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    changed = c;
    changed.challenge_id += "overflow";
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);
    changed = c;
    changed.approval_reference = std::string(200, 'a');
    assert(inspect(s, p, changed, statement, key.public_key)
        == Code::invalid_challenge);

    auto altered_p = p;
    altered_p.approval_reference = "approval-other";
    assert(inspect(s, altered_p, c, statement, key.public_key)
        == Code::invalid_proposal_or_scope);
    auto altered_s = s;
    altered_s.retry_generation = 2;
    assert(inspect(altered_s, p, c, statement, key.public_key)
        == Code::invalid_proposal_or_scope);
}

void test_bad_challenge_request_fails_closed()
{
    const auto s = scope();
    const auto p = proposal(s);
    const auto make = [&](const std::string& nonce,
                          std::int64_t issued, std::int64_t ttl) {
        return make_local_goose_dialogue_responder_retry_operator_challenge(
            p, s, nonce, issued, ttl);
    };
    const std::string nonce(64, 'a');
    assert(!make(std::string(64, '0'), 4000, 10000));
    assert(!make(std::string(64, 'Z'), 4000, 10000));
    assert(!make("not-a-nonce", 4000, 10000));
    assert(!make(nonce, 2999, 10000));
    assert(!make(nonce, 4000, 0));
    assert(!make(nonce, 4000, 300001));
    assert(!make(nonce, p.expires_at_ms - 1, 2));
    assert(!make(nonce, p.expires_at_ms, 1));
    assert(make(nonce, 4000, 300000 - 1000));
    assert(!make(nonce, 4000, 300001));
}

} // namespace

int main()
{
    test_exact_pinned_key_and_statement();
    test_exact_scope_nonce_and_challenge_binding();
    test_bad_challenge_request_fails_closed();
    return 0;
}
