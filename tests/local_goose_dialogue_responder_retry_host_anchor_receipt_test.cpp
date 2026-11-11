#include "LocalGooseDialogueResponderRetryHostAnchorReceipt.hpp"

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
using Code = LocalGooseDialogueResponderRetryHostReceiptCode;
using Anchor = LocalGooseDialogueResponderRetryRootOwnedAnchorRecord;

struct SyntheticHost {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> priv {
        nullptr, EVP_PKEY_free
    };
    explicit SyntheticHost(const unsigned char seed_byte)
    {
        // Test-only deterministic Ed25519 material. NEVER install this key.
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

std::string signer_fingerprint(const SyntheticHost& key)
{
    const std::string bytes(
        reinterpret_cast<const char*>(key.pub.data()), key.pub.size());
    return sha256_hex("gaudere-retry-host-attestor-ed25519-key-v1|" + bytes);
}

std::string sign(SyntheticHost& key, const std::string& bytes)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(ctx);
    assert(EVP_DigestSignInit(
        ctx.get(), nullptr, nullptr, nullptr, key.priv.get()) == 1);
    std::array<unsigned char, 64> sig {};
    std::size_t n = sig.size();
    assert(EVP_DigestSign(ctx.get(), sig.data(), &n,
        reinterpret_cast<const unsigned char*>(bytes.data()),
        bytes.size()) == 1);
    assert(n == sig.size());
    constexpr char alphabet[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out += alphabet[sig[i] >> 4];
        out += alphabet[sig[i] & 15];
    }
    return out;
}

Anchor observed()
{
    Anchor a;
    a.root_pin_path = "/tmp/gaudere-ci-private/root.pin";
    a.root_fingerprint = std::string(64, 'a');
    a.operator_key_fingerprint = std::string(64, 'b');
    a.anchor_epoch = 7;
    return a;
}

std::string body(const Anchor& anchor, const std::string& context,
                 const std::string& epoch = "7",
                 const std::string& from = "1000",
                 const std::string& until = "100000")
{
    return std::string(local_goose_dialogue_responder_retry_host_receipt_header)
        + "root_pin_path=" + anchor.root_pin_path + "\n"
        + "root_fingerprint=" + anchor.root_fingerprint + "\n"
        + "operator_key_fingerprint=" + anchor.operator_key_fingerprint + "\n"
        + "anchor_epoch=" + epoch + "\n"
        + "host_context_sha256=" + context + "\n"
        + "not_before_ms=" + from + "\n"
        + "not_after_ms=" + until + "\n";
}

std::string receipt(SyntheticHost& key, const std::string& unsigned_body)
{
    return unsigned_body + "signature_hex=" + sign(key,
        std::string(local_goose_dialogue_responder_retry_host_receipt_domain)
        + unsigned_body) + "\n";
}

Code inspect(const std::string& candidate, const SyntheticHost& expected,
             const Anchor& anchor, const std::string& context,
             const std::uint64_t minimum = 7,
             const std::int64_t now = 5000,
             const std::string& fp_override = "")
{
    return inspect_local_goose_dialogue_responder_retry_host_anchor_receipt(
        candidate, expected.pub,
        fp_override.empty() ? signer_fingerprint(expected) : fp_override,
        context, anchor, minimum, now);
}

void test_independent_synthetic_host_signature_and_exact_manifest()
{
    SyntheticHost host(0x30);
    SyntheticHost hostile(0x31);
    const auto anchor = observed();
    const std::string context(64, 'c');
    const auto canonical = receipt(host, body(anchor, context));

    assert(inspect(canonical, host, anchor, context)
        == Code::cryptographically_consistent_no_authority);
    assert(inspect(canonical, host, anchor, context)
        == Code::cryptographically_consistent_no_authority);
    assert(inspect(canonical, host, anchor, context, 8)
        == Code::stale_or_outside_validity);
    assert(inspect(canonical, host, anchor, context, 7, 999)
        == Code::stale_or_outside_validity);
    assert(inspect(canonical, host, anchor, context, 7, 100000)
        == Code::stale_or_outside_validity);

    assert(inspect(canonical, host, anchor, context, 0)
        == Code::invalid_independent_host_trust);
    assert(inspect(canonical, host, anchor, context, 7, 5000,
                   std::string(64, '0'))
        == Code::invalid_independent_host_trust);
    assert(inspect(canonical, host, anchor, std::string(64, '0'))
        == Code::invalid_independent_host_trust);

    // A caller may sign its OWN receipt, but that does not make it the
    // separately pinned host identity. Never trust candidate-supplied signer.
    const auto self_attested = receipt(hostile, body(anchor, context));
    assert(inspect(self_attested, host, anchor, context)
        == Code::invalid_host_signature);

    // Different purpose prefixes alone do not prove independent custody:
    // a host signing key must not ALSO be the trust root or operator key.
    const std::string host_raw(
        reinterpret_cast<const char*>(host.pub.data()), host.pub.size());
    Anchor same_key = anchor;
    same_key.root_fingerprint = sha256_hex(
        "gaudere-retry-operator-root-ed25519-key-v1|" + host_raw);
    assert(inspect(receipt(host, body(same_key, context)),
                   host, same_key, context)
        == Code::invalid_independent_host_trust);
    same_key = anchor;
    same_key.operator_key_fingerprint = sha256_hex(
        "gaudere-retry-operator-ed25519-key-v1|" + host_raw);
    assert(inspect(receipt(host, body(same_key, context)),
                   host, same_key, context)
        == Code::invalid_independent_host_trust);
    assert(inspect(canonical, hostile, anchor, context)
        == Code::invalid_host_signature);
    assert(inspect_local_goose_dialogue_responder_retry_host_anchor_receipt(
        canonical, hostile.pub, signer_fingerprint(host), context, anchor, 7, 5000)
        == Code::invalid_independent_host_trust);

    auto tampered = canonical;
    const auto at = tampered.find("root_fingerprint=");
    assert(at != std::string::npos);
    tampered[at + std::string("root_fingerprint=").size()] = 'd';
    assert(inspect(tampered, host, anchor, context)
        == Code::invalid_host_signature);

    Anchor other = anchor;
    other.root_pin_path = "/tmp/foreign-private/root.pin";
    assert(inspect(canonical, host, other, context)
        == Code::anchor_or_host_context_mismatch);
    other = anchor;
    other.root_fingerprint = std::string(64, 'd');
    assert(inspect(canonical, host, other, context)
        == Code::anchor_or_host_context_mismatch);
    other = anchor;
    other.operator_key_fingerprint = std::string(64, 'd');
    assert(inspect(canonical, host, other, context)
        == Code::anchor_or_host_context_mismatch);
    other = anchor;
    other.anchor_epoch = 8;
    assert(inspect(canonical, host, other, context)
        == Code::anchor_or_host_context_mismatch);
    assert(inspect(canonical, host, anchor, std::string(64, 'd'))
        == Code::anchor_or_host_context_mismatch);
}

void test_canonical_wire_constraints_and_domain_separation()
{
    SyntheticHost host(0x30);
    Anchor anchor = observed();
    const std::string context(64, 'c');
    const auto canon = receipt(host, body(anchor, context));
    const auto base = body(anchor, context);

    assert(inspect(canon + "\n", host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(canon.substr(0, canon.size() - 1), host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(std::string(1700, 'x'), host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(receipt(host, base + "unknown=1\n"), host, anchor, context)
        == Code::invalid_receipt_format);
    auto noncanonical = base;
    const auto at = noncanonical.find("anchor_epoch=7\n");
    assert(at != std::string::npos);
    noncanonical.replace(at, std::string("anchor_epoch=7\n").size(),
                         "anchor_epoch=07\n");
    assert(inspect(receipt(host, noncanonical), host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(receipt(host, body(anchor, context, "9223372036854775808")),
                   host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(receipt(host, body(anchor, context, "7", "0")),
                   host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(receipt(host, body(anchor, context, "7", "1000", "1000")),
                   host, anchor, context)
        == Code::invalid_receipt_format);
    assert(inspect(receipt(host, body(anchor, context, "7", "1000",
                   "3000000000")), host, anchor, context)
        == Code::invalid_receipt_format);

    anchor.root_pin_path = "/tmp/../unsafe.pin";
    assert(inspect(receipt(host, body(anchor, context)), host, anchor, context)
        == Code::invalid_receipt_format);
    anchor = observed();
    anchor.root_pin_path = "/tmp/./unsafe.pin";
    assert(inspect(receipt(host, body(anchor, context)), host, anchor, context)
        == Code::invalid_receipt_format);
    anchor = observed();
    anchor.root_fingerprint = std::string(64, 'A');
    assert(inspect(receipt(host, body(anchor, context)), host, anchor, context)
        == Code::invalid_receipt_format);

    // Different signing domain must never pass.
    const auto wrongly_signed = base + "signature_hex=" + sign(host, base) + "\n";
    assert(inspect(wrongly_signed, host, observed(), context)
        == Code::invalid_host_signature);
    const auto nonce_free = base + "signature_hex=" + std::string(128, '0') + "\n";
    assert(inspect(nonce_free, host, observed(), context)
        == Code::invalid_receipt_format);
}

} // namespace

int main()
{
    test_independent_synthetic_host_signature_and_exact_manifest();
    test_canonical_wire_constraints_and_domain_separation();
    return 0;
}
