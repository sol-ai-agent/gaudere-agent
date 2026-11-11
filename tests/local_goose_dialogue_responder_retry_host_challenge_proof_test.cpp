#include "LocalGooseDialogueResponderRetryHostChallengeProof.hpp"

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
using Code = LocalGooseDialogueResponderRetryHostChallengeCode;
using Anchor = LocalGooseDialogueResponderRetryRootOwnedAnchorRecord;

struct SyntheticKey {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> priv {
        nullptr, EVP_PKEY_free
    };
    explicit SyntheticKey(const unsigned char seed_byte)
    {
        // CI-only deterministic seed: no enrolled or operational key.
        std::array<unsigned char, 32> seed {};
        seed.fill(seed_byte);
        priv.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(priv);
        std::size_t n = pub.size();
        assert(EVP_PKEY_get_raw_public_key(priv.get(), pub.data(), &n) == 1);
        assert(n == pub.size());
    }
};

std::string hex(const unsigned char* bytes, const std::size_t count)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    result.reserve(count * 2);
    for (std::size_t i = 0; i < count; ++i) {
        result.push_back(alphabet[bytes[i] >> 4]);
        result.push_back(alphabet[bytes[i] & 15]);
    }
    return result;
}

std::string sign(SyntheticKey& key, const std::string& bytes)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(context);
    assert(EVP_DigestSignInit(context.get(), nullptr, nullptr,
                               nullptr, key.priv.get()) == 1);
    std::array<unsigned char, 64> sig {};
    std::size_t n = sig.size();
    assert(EVP_DigestSign(context.get(), sig.data(), &n,
        reinterpret_cast<const unsigned char*>(bytes.data()),
        bytes.size()) == 1);
    assert(n == sig.size());
    return hex(sig.data(), n);
}

std::string host_fp(const SyntheticKey& host)
{
    const std::string raw(
        reinterpret_cast<const char*>(host.pub.data()), host.pub.size());
    return sha256_hex("gaudere-retry-host-attestor-ed25519-key-v1|" + raw);
}

Anchor observed()
{
    Anchor a;
    a.root_pin_path = "/tmp/gaudere-test-protected/root.pin";
    a.root_fingerprint = std::string(64, 'a');
    a.operator_key_fingerprint = std::string(64, 'b');
    a.anchor_epoch = 7;
    return a;
}

std::string v1_body(const Anchor& a, const std::string& host_context)
{
    return std::string(local_goose_dialogue_responder_retry_host_receipt_header)
        + "root_pin_path=" + a.root_pin_path + "\n"
        + "root_fingerprint=" + a.root_fingerprint + "\n"
        + "operator_key_fingerprint=" + a.operator_key_fingerprint + "\n"
        + "anchor_epoch=" + std::to_string(a.anchor_epoch) + "\n"
        + "host_context_sha256=" + host_context + "\n"
        + "not_before_ms=1000\n"
        + "not_after_ms=2000000\n";
}

std::string v1(SyntheticKey& host, const Anchor& a, const std::string& context)
{
    const auto body = v1_body(a, context);
    return body + "signature_hex=" + sign(host,
        std::string(local_goose_dialogue_responder_retry_host_receipt_domain)
            + body) + "\n";
}

std::string v2_body(const std::string& v1_receipt,
                    const std::string& challenge,
                    const std::string& request,
                    const std::string& start = "3000",
                    const std::string& end = "180000")
{
    return std::string(local_goose_dialogue_responder_retry_host_challenge_header)
        + "receipt_sha256=" + sha256_hex(v1_receipt) + "\n"
        + "challenge_sha256=" + challenge + "\n"
        + "request_context_sha256=" + request + "\n"
        + "not_before_ms=" + start + "\n"
        + "not_after_ms=" + end + "\n";
}

std::string v2(SyntheticKey& host, const std::string& body)
{
    return body + "signature_hex=" + sign(host,
        std::string(local_goose_dialogue_responder_retry_host_challenge_domain)
        + body) + "\n";
}

Code check(const std::string& v1_receipt, const std::string& v2_proof,
           const SyntheticKey& host, const Anchor& anchor,
           const std::string& challenge, const std::string& request,
           const std::int64_t now = 5000,
           const std::uint64_t minimum_epoch = 7,
           const std::string& host_context = std::string(64, 'c'))
{
    return inspect_local_goose_dialogue_responder_retry_host_challenge_proof(
        v1_receipt, v2_proof, host.pub, host_fp(host),
        host_context, anchor, minimum_epoch, challenge, request, now);
}

void test_exact_receipt_and_externally_expected_challenge()
{
    SyntheticKey host(0x30), foreign(0x31);
    const auto anchor = observed();
    const std::string host_context(64, 'c');
    const std::string challenge(64, 'd');
    const std::string request(64, 'e');
    const auto receipt = v1(host, anchor, host_context);
    const auto proof = v2(host, v2_body(receipt, challenge, request));

    assert(check(receipt, proof, host, anchor, challenge, request)
        == Code::challenge_bound_cryptographic_consistency_no_authority);
    // Repeating identical inputs is NOT an anti-replay guarantee.
    assert(check(receipt, proof, host, anchor, challenge, request)
        == Code::challenge_bound_cryptographic_consistency_no_authority);
    assert(check(receipt, proof, host, anchor, std::string(64, 'f'), request)
        == Code::wrong_exact_receipt_or_challenge_or_request);
    assert(check(receipt, proof, host, anchor, challenge, std::string(64, 'f'))
        == Code::wrong_exact_receipt_or_challenge_or_request);
    assert(check(receipt, proof, host, anchor, challenge, request, 2999)
        == Code::stale_or_outside_validity);
    assert(check(receipt, proof, host, anchor, challenge, request, 180000)
        == Code::stale_or_outside_validity);
    assert(check(receipt, proof, host, anchor, challenge, request, 5000, 8)
        == Code::invalid_base_host_receipt);

    Anchor later = anchor;
    later.anchor_epoch = 8;
    const auto new_receipt = v1(host, later, host_context);
    assert(check(new_receipt, proof, host, later, challenge, request, 5000, 8)
        == Code::wrong_exact_receipt_or_challenge_or_request);
    const auto valid_new_proof =
        v2(host, v2_body(new_receipt, challenge, request));
    assert(check(new_receipt, valid_new_proof, host, later,
                 challenge, request, 5000, 8)
        == Code::challenge_bound_cryptographic_consistency_no_authority);

    assert(check(receipt, proof, foreign, anchor, challenge, request)
        == Code::invalid_base_host_receipt);
    assert(check(v1(foreign, anchor, host_context), proof, host,
                 anchor, challenge, request)
        == Code::invalid_base_host_receipt);
    assert(check(receipt, v2(foreign, v2_body(receipt, challenge, request)),
                 host, anchor, challenge, request)
        == Code::invalid_host_challenge_signature);
    auto tampered = proof;
    const auto sig = tampered.find("signature_hex=");
    assert(sig != std::string::npos);
    tampered[sig + std::string("signature_hex=").size()] =
        tampered[sig + std::string("signature_hex=").size()] == '1' ? '2' : '1';
    assert(check(receipt, tampered, host, anchor, challenge, request)
        == Code::invalid_host_challenge_signature);
    assert(check(receipt, proof, host, anchor, std::string(64, '0'), request)
        == Code::invalid_independent_trust_or_expectation);
    assert(check(receipt, proof, host, anchor, challenge, "invalid")
        == Code::invalid_independent_trust_or_expectation);
}

void test_canonical_format_and_short_lifetime()
{
    SyntheticKey host(0x30);
    const auto anchor = observed();
    const std::string challenge(64, 'd'), request(64, 'e');
    const auto receipt = v1(host, anchor, std::string(64, 'c'));
    const auto body = v2_body(receipt, challenge, request);
    const auto canonical = v2(host, body);
    const auto bad = [&](const std::string& candidate) {
        return check(receipt, candidate, host, anchor, challenge, request);
    };

    assert(bad(canonical + "\n") == Code::invalid_challenge_format);
    assert(bad(canonical.substr(0, canonical.size() - 1))
        == Code::invalid_challenge_format);
    assert(bad(std::string(1025, 'x')) == Code::invalid_challenge_format);
    assert(bad(v2(host, body + "unexpected=1\n"))
        == Code::invalid_challenge_format);
    auto crlf = body;
    crlf.insert(crlf.find('\n'), "\r");
    assert(bad(v2(host, crlf)) == Code::invalid_challenge_format);
    auto tabbed = body;
    tabbed.insert(tabbed.find("challenge_sha256=") + 17, "\t");
    assert(bad(v2(host, tabbed)) == Code::invalid_challenge_format);
    auto nonascii = body;
    nonascii.insert(nonascii.find("challenge_sha256=") + 17, "\xc3\xa9");
    assert(bad(v2(host, nonascii)) == Code::invalid_challenge_format);
    assert(bad(v2(host, v2_body(receipt, challenge, request,
                                "3000", "303001")))
        == Code::invalid_challenge_format); // over 5 minutes
    assert(bad(v2(host, v2_body(receipt, challenge, request, "0", "3000")))
        == Code::invalid_challenge_format);
    assert(bad(v2(host, v2_body(receipt, challenge, request, "03000", "180000")))
        == Code::invalid_challenge_format);
    assert(bad(v2(host, v2_body(receipt, challenge, request, "3000", "3000")))
        == Code::invalid_challenge_format);
    assert(bad(v2(host, v2_body(receipt, challenge, request,
                                "9223372036854775808", "9223372036854775809")))
        == Code::invalid_challenge_format);
    assert(bad(body + "signature_hex=" + sign(host, body) + "\n")
        == Code::invalid_host_challenge_signature);
    assert(bad(body + "signature_hex=" + std::string(128, '0') + "\n")
        == Code::invalid_challenge_format);
}

} // namespace

int main()
{
    test_exact_receipt_and_externally_expected_challenge();
    test_canonical_format_and_short_lifetime();
    return 0;
}
