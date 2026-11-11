#include "LocalGooseDialogueResponderRetryHostChallengeProof.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryHostChallengeCode;
constexpr std::size_t max_proof_bytes = 1024;
constexpr std::int64_t max_challenge_lifetime_ms = 300000;

struct ParsedChallenge {
    std::string exact_receipt_sha256;
    std::string challenge_sha256;
    std::string request_context_sha256;
    std::int64_t not_before_ms = 0;
    std::int64_t not_after_ms = 0;
    std::array<unsigned char, 64> signature {};
    std::string signed_message;
};

bool nonzero_lower_hex(const std::string& value, const std::size_t expected)
{
    if (value.size() != expected) return false;
    bool nonzero = false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

unsigned char digit(const char c) noexcept
{
    return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
}

std::optional<std::int64_t> decimal(const std::string& raw)
{
    if (raw.empty() || raw.size() > 19
        || (raw.size() > 1 && raw[0] == '0'))
        return std::nullopt;
    constexpr std::uint64_t upper =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    std::uint64_t value = 0;
    for (const unsigned char c : raw) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto n = static_cast<std::uint64_t>(c - '0');
        if (value > (upper - n) / 10u) return std::nullopt;
        value = value * 10u + n;
    }
    return static_cast<std::int64_t>(value);
}

std::optional<std::string> field(const std::string& exact,
                                 std::size_t& offset, const char* name)
{
    const std::string prefix(name);
    if (exact.compare(offset, prefix.size(), prefix) != 0)
        return std::nullopt;
    const auto begin = offset + prefix.size();
    const auto end = exact.find('\n', begin);
    if (end == std::string::npos) return std::nullopt;
    offset = end + 1;
    return exact.substr(begin, end - begin);
}

std::optional<ParsedChallenge> parse(const std::string& exact)
{
    const std::string header(
        local_goose_dialogue_responder_retry_host_challenge_header);
    if (exact.size() > max_proof_bytes
        || exact.compare(0, header.size(), header) != 0)
        return std::nullopt;
    // The signature covers exact canonical printable ASCII plus LF bytes.
    for (const unsigned char c : exact)
        if ((c < 0x20 && c != '\n') || c > 0x7e)
            return std::nullopt;

    std::size_t offset = header.size();
    const auto receipt = field(exact, offset, "receipt_sha256=");
    const auto challenge = field(exact, offset, "challenge_sha256=");
    const auto request = field(exact, offset, "request_context_sha256=");
    const auto start = field(exact, offset, "not_before_ms=");
    const auto end = field(exact, offset, "not_after_ms=");
    if (!receipt || !challenge || !request || !start || !end)
        return std::nullopt;

    const std::size_t signed_end = offset;
    const auto sig = field(exact, offset, "signature_hex=");
    if (!sig || offset != exact.size()
        || !nonzero_lower_hex(*receipt, 64)
        || !nonzero_lower_hex(*challenge, 64)
        || !nonzero_lower_hex(*request, 64)
        || !nonzero_lower_hex(*sig, 128))
        return std::nullopt;
    const auto from = decimal(*start);
    const auto until = decimal(*end);
    if (!from || !until || *from <= 0 || *until <= *from
        || *until - *from > max_challenge_lifetime_ms)
        return std::nullopt;

    ParsedChallenge result;
    result.exact_receipt_sha256 = *receipt;
    result.challenge_sha256 = *challenge;
    result.request_context_sha256 = *request;
    result.not_before_ms = *from;
    result.not_after_ms = *until;
    result.signed_message =
        std::string(local_goose_dialogue_responder_retry_host_challenge_domain)
        + exact.substr(0, signed_end);
    for (std::size_t i = 0; i < result.signature.size(); ++i)
        result.signature[i] = static_cast<unsigned char>(
            (digit((*sig)[2*i]) << 4u) | digit((*sig)[2*i + 1]));
    return result;
}

bool verify(const ParsedChallenge& proof,
            const std::array<unsigned char, 32>& host_public_key)
{
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                host_public_key.data(), host_public_key.size()), EVP_PKEY_free);
    if (!key) return false;
    Ctx ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return ctx
        && EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestVerify(ctx.get(),
            proof.signature.data(), proof.signature.size(),
            reinterpret_cast<const unsigned char*>(proof.signed_message.data()),
            proof.signed_message.size()) == 1;
}

} // namespace

Code inspect_local_goose_dialogue_responder_retry_host_challenge_proof(
    const std::string& exact_v1_host_signed_receipt,
    const std::string& candidate_exact_v2_host_challenge_proof,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    const std::uint64_t independently_trusted_minimum_anchor_epoch,
    const std::string& independently_expected_challenge_sha256,
    const std::string& independently_expected_request_context_sha256,
    const std::int64_t trusted_now_ms)
{
    try {
        if (!nonzero_lower_hex(independently_expected_challenge_sha256, 64)
            || !nonzero_lower_hex(independently_expected_request_context_sha256, 64))
            return Code::invalid_independent_trust_or_expectation;

        // Re-verify the EXACT v1 receipt instead of trusting an unverified
        // receipt hash or an untrusted pre-parsed host identity.
        if (inspect_local_goose_dialogue_responder_retry_host_anchor_receipt(
                exact_v1_host_signed_receipt,
                independently_pinned_host_signer_public_key,
                independently_pinned_host_signer_fingerprint,
                independently_expected_host_context_sha256,
                observed_anchor, independently_trusted_minimum_anchor_epoch,
                trusted_now_ms)
            != LocalGooseDialogueResponderRetryHostReceiptCode::
                    cryptographically_consistent_no_authority)
            return Code::invalid_base_host_receipt;

        const auto proof = parse(candidate_exact_v2_host_challenge_proof);
        if (!proof) return Code::invalid_challenge_format;
        if (!verify(*proof, independently_pinned_host_signer_public_key))
            return Code::invalid_host_challenge_signature;

        if (proof->exact_receipt_sha256 != sha256_hex(exact_v1_host_signed_receipt)
            || proof->challenge_sha256
                != independently_expected_challenge_sha256
            || proof->request_context_sha256
                != independently_expected_request_context_sha256)
            return Code::wrong_exact_receipt_or_challenge_or_request;

        if (trusted_now_ms < proof->not_before_ms
            || trusted_now_ms >= proof->not_after_ms)
            return Code::stale_or_outside_validity;

        // No nonce consumption; a caller replaying both the proof and its
        // supplied expected challenge will still receive ONLY an inert result.
        return Code::challenge_bound_cryptographic_consistency_no_authority;
    } catch (...) {
        return Code::invalid_challenge_format;
    }
}

} // namespace gaudere_agent
