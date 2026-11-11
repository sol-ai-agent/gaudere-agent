#include "LocalGooseDialogueResponderRetryHostAnchorReceipt.hpp"

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

using Code = LocalGooseDialogueResponderRetryHostReceiptCode;
constexpr std::size_t max_receipt_size = 1600;
constexpr std::int64_t max_lifetime_ms = 31LL * 24 * 60 * 60 * 1000;

struct ParsedReceipt {
    std::string root_pin_path;
    std::string root_fingerprint;
    std::string operator_key_fingerprint;
    std::uint64_t anchor_epoch = 0;
    std::string host_context_sha256;
    std::int64_t not_before_ms = 0;
    std::int64_t not_after_ms = 0;
    std::array<unsigned char, 64> signature {};
    std::string signed_payload;
};

bool lowercase_hex(const std::string& value, const std::size_t count) noexcept
{
    if (value.size() != count) return false;
    bool nonzero = false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool canonical_path(const std::string& path) noexcept
{
    if (path.size() < 3 || path.size() > 512
        || path.front() != '/' || path.find('\0') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const std::size_t length =
            (end == std::string::npos ? path.size() : end) - start;
        if (length == 0 || (length == 1 && path[start] == '.')
            || (length == 2 && path.compare(start, 2, "..") == 0))
            return false;
        if (end == std::string::npos) return true;
        start = end + 1;
    }
    return false;
}

unsigned char hex_value(const char value) noexcept
{
    return static_cast<unsigned char>(
        value <= '9' ? value - '0' : value - 'a' + 10);
}

std::optional<std::uint64_t> decimal(const std::string& raw)
{
    constexpr auto limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (raw.empty() || raw.size() > 19
        || (raw.size() > 1 && raw.front() == '0'))
        return std::nullopt;
    std::uint64_t result = 0;
    for (const unsigned char digit : raw) {
        if (digit < '0' || digit > '9') return std::nullopt;
        const std::uint64_t n = digit - '0';
        if (result > (limit - n) / 10u) return std::nullopt;
        result = result * 10u + n;
    }
    return result;
}

std::optional<std::string> field(const std::string& raw,
                                 std::size_t& offset, const char* label)
{
    const std::string prefix(label);
    if (raw.compare(offset, prefix.size(), prefix) != 0)
        return std::nullopt;
    const auto begin = offset + prefix.size();
    const auto end = raw.find('\n', begin);
    if (end == std::string::npos) return std::nullopt;
    offset = end + 1;
    return raw.substr(begin, end - begin);
}

std::optional<ParsedReceipt> parse(const std::string& exact)
{
    const std::string header(
        local_goose_dialogue_responder_retry_host_receipt_header);
    if (exact.size() > max_receipt_size
        || exact.find('\0') != std::string::npos
        || exact.compare(0, header.size(), header) != 0)
        return std::nullopt;
    std::size_t offset = header.size();
    const auto pin = field(exact, offset, "root_pin_path=");
    const auto root = field(exact, offset, "root_fingerprint=");
    const auto op = field(exact, offset, "operator_key_fingerprint=");
    const auto epoch = field(exact, offset, "anchor_epoch=");
    const auto context = field(exact, offset, "host_context_sha256=");
    const auto from = field(exact, offset, "not_before_ms=");
    const auto until = field(exact, offset, "not_after_ms=");
    if (!pin || !root || !op || !epoch || !context || !from || !until)
        return std::nullopt;

    const std::size_t signed_end = offset;
    const auto signature_hex = field(exact, offset, "signature_hex=");
    if (!signature_hex || offset != exact.size()
        || !canonical_path(*pin)
        || !lowercase_hex(*root, 64) || !lowercase_hex(*op, 64)
        || !lowercase_hex(*context, 64)
        || !lowercase_hex(*signature_hex, 128))
        return std::nullopt;
    const auto epoch_number = decimal(*epoch);
    const auto start_number = decimal(*from);
    const auto end_number = decimal(*until);
    if (!epoch_number || *epoch_number == 0
        || !start_number || !end_number || *start_number == 0
        || *end_number <= *start_number
        || *end_number - *start_number
            > static_cast<std::uint64_t>(max_lifetime_ms))
        return std::nullopt;

    ParsedReceipt parsed;
    parsed.root_pin_path = *pin;
    parsed.root_fingerprint = *root;
    parsed.operator_key_fingerprint = *op;
    parsed.anchor_epoch = *epoch_number;
    parsed.host_context_sha256 = *context;
    parsed.not_before_ms = static_cast<std::int64_t>(*start_number);
    parsed.not_after_ms = static_cast<std::int64_t>(*end_number);
    parsed.signed_payload =
        std::string(local_goose_dialogue_responder_retry_host_receipt_domain)
        + exact.substr(0, signed_end);
    for (std::size_t i = 0; i < parsed.signature.size(); ++i)
        parsed.signature[i] = static_cast<unsigned char>(
            (hex_value((*signature_hex)[2*i]) << 4u)
            | hex_value((*signature_hex)[2*i + 1]));
    return parsed;
}

bool valid_signature(const ParsedReceipt& receipt,
                     const std::array<unsigned char, 32>& host_key)
{
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

    Key key(EVP_PKEY_new_raw_public_key(
                EVP_PKEY_ED25519, nullptr, host_key.data(), host_key.size()),
            EVP_PKEY_free);
    if (!key) return false;
    Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return ctx && EVP_DigestVerifyInit(
            ctx.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestVerify(ctx.get(),
            receipt.signature.data(), receipt.signature.size(),
            reinterpret_cast<const unsigned char*>(receipt.signed_payload.data()),
            receipt.signed_payload.size()) == 1;
}

} // namespace

Code inspect_local_goose_dialogue_responder_retry_host_anchor_receipt(
    const std::string& candidate_exact_signed_receipt,
    const std::array<unsigned char, 32>& independently_pinned_host_signer_public_key,
    const std::string& independently_pinned_host_signer_fingerprint,
    const std::string& independently_expected_host_context_sha256,
    const LocalGooseDialogueResponderRetryRootOwnedAnchorRecord& observed_anchor,
    const std::uint64_t independently_trusted_minimum_anchor_epoch,
    const std::int64_t trusted_now_ms)
{
    try {
        if (!lowercase_hex(independently_pinned_host_signer_fingerprint, 64)
            || !lowercase_hex(independently_expected_host_context_sha256, 64)
            || independently_trusted_minimum_anchor_epoch == 0) {
            return Code::invalid_independent_host_trust;
        }
        const std::string raw(
            reinterpret_cast<const char*>(independently_pinned_host_signer_public_key.data()),
            independently_pinned_host_signer_public_key.size());
        if (sha256_hex("gaudere-retry-host-attestor-ed25519-key-v1|" + raw)
                != independently_pinned_host_signer_fingerprint)
            return Code::invalid_independent_host_trust;
        // Domain separation does not make the SAME Ed25519 private key a
        // separate identity. Forbid host signer reuse as root or operator.
        if (sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw)
                == observed_anchor.root_fingerprint
            || sha256_hex("gaudere-retry-operator-ed25519-key-v1|" + raw)
                == observed_anchor.operator_key_fingerprint)
            return Code::invalid_independent_host_trust;

        const auto receipt = parse(candidate_exact_signed_receipt);
        if (!receipt) return Code::invalid_receipt_format;
        if (!valid_signature(*receipt, independently_pinned_host_signer_public_key))
            return Code::invalid_host_signature;

        if (receipt->root_pin_path != observed_anchor.root_pin_path
            || receipt->root_fingerprint != observed_anchor.root_fingerprint
            || receipt->operator_key_fingerprint
                != observed_anchor.operator_key_fingerprint
            || receipt->anchor_epoch != observed_anchor.anchor_epoch
            || receipt->host_context_sha256
                != independently_expected_host_context_sha256)
            return Code::anchor_or_host_context_mismatch;

        if (receipt->anchor_epoch < independently_trusted_minimum_anchor_epoch
            || trusted_now_ms < receipt->not_before_ms
            || trusted_now_ms >= receipt->not_after_ms)
            return Code::stale_or_outside_validity;

        return Code::cryptographically_consistent_no_authority;
    } catch (...) {
        return Code::invalid_receipt_format;
    }
}

} // namespace gaudere_agent
