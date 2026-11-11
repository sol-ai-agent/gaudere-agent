#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"

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

using Code = LocalGooseDialogueResponderRetryTrustSnapshotCode;
constexpr const char* signing_domain =
    "gaudere-retry-operator-trust-snapshot-signature.v1|";
constexpr std::int64_t max_snapshot_lifetime_ms = 366LL * 24 * 60 * 60 * 1000;
constexpr std::size_t max_snapshot_bytes = 1024;

struct Snapshot {
    std::string key_fingerprint;
    std::string status;
    std::uint64_t generation = 0;
    std::int64_t not_before_ms = -1;
    std::int64_t not_after_ms = -1;
    std::array<unsigned char, 64> signature {};
    std::string signed_bytes;
};

bool lower_hex(const std::string& value, const std::size_t len) noexcept
{
    if (value.size() != len) return false;
    for (const unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

unsigned char hex_digit(const char c) noexcept
{
    return static_cast<unsigned char>(
        c <= '9' ? c - '0' : c - 'a' + 10);
}

std::optional<std::uint64_t> decimal(const std::string& value,
                                     const std::uint64_t upper)
{
    if (value.empty() || (value.size() > 1 && value[0] == '0'))
        return std::nullopt;
    std::uint64_t n = 0;
    for (const unsigned char c : value) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = c - '0';
        if (n > (upper - digit) / 10u) return std::nullopt;
        n = n * 10u + digit;
    }
    return n;
}

std::optional<std::string> extract(const std::string& raw,
                                   std::size_t& offset, const char* label)
{
    const std::string prefix(label);
    if (raw.compare(offset, prefix.size(), prefix) != 0)
        return std::nullopt;
    const std::size_t begin = offset + prefix.size();
    const std::size_t end = raw.find('\n', begin);
    if (end == std::string::npos) return std::nullopt;
    offset = end + 1;
    return raw.substr(begin, end - begin);
}

std::optional<Snapshot> parse(const std::string& raw)
{
    const std::string header(local_goose_dialogue_responder_retry_trust_snapshot_header);
    if (raw.size() > max_snapshot_bytes
        || raw.compare(0, header.size(), header) != 0)
        return std::nullopt;

    std::size_t offset = header.size();
    const auto fingerprint = extract(raw, offset, "key_fingerprint=");
    const auto status = extract(raw, offset, "status=");
    const auto generation = extract(raw, offset, "generation=");
    const auto from = extract(raw, offset, "not_before_ms=");
    const auto until = extract(raw, offset, "not_after_ms=");
    if (!fingerprint || !status || !generation || !from || !until)
        return std::nullopt;
    const std::size_t signed_end = offset;
    const auto signature_hex = extract(raw, offset, "signature_hex=");
    if (!signature_hex || offset != raw.size()
        || !lower_hex(*fingerprint, 64)
        || (*status != "active" && *status != "revoked")
        || !lower_hex(*signature_hex, 128))
        return std::nullopt;

    const auto gen = decimal(*generation, std::numeric_limits<std::int64_t>::max());
    const auto from_ms = decimal(*from, std::numeric_limits<std::int64_t>::max());
    const auto until_ms = decimal(*until, std::numeric_limits<std::int64_t>::max());
    if (!gen || !from_ms || !until_ms || *gen == 0
        || *until_ms <= *from_ms
        || *until_ms - *from_ms > static_cast<std::uint64_t>(max_snapshot_lifetime_ms))
        return std::nullopt;

    Snapshot result;
    result.key_fingerprint = *fingerprint;
    result.status = *status;
    result.generation = *gen;
    result.not_before_ms = static_cast<std::int64_t>(*from_ms);
    result.not_after_ms = static_cast<std::int64_t>(*until_ms);
    result.signed_bytes = std::string(signing_domain) + raw.substr(0, signed_end);

    for (std::size_t i = 0; i < result.signature.size(); ++i)
        result.signature[i] = static_cast<unsigned char>(
            (hex_digit((*signature_hex)[2*i]) << 4u)
            | hex_digit((*signature_hex)[2*i+1]));
    return result;
}

bool verify_root(const Snapshot& snapshot,
                 const std::array<unsigned char, 32>& root)
{
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

    Key key(EVP_PKEY_new_raw_public_key(
                EVP_PKEY_ED25519, nullptr, root.data(), root.size()),
            EVP_PKEY_free);
    if (!key) return false;
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return context
        && EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1
        && EVP_DigestVerify(context.get(),
               snapshot.signature.data(), snapshot.signature.size(),
               reinterpret_cast<const unsigned char*>(snapshot.signed_bytes.data()),
               snapshot.signed_bytes.size()) == 1;
}

std::string root_fingerprint(const std::array<unsigned char, 32>& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.data()), key.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

} // namespace

Code inspect_local_goose_dialogue_responder_retry_operator_trust_snapshot(
    const std::string& candidate_signed_snapshot,
    const std::array<unsigned char, 32>& separately_enrolled_root_public_key,
    const std::string& independently_trusted_root_fingerprint,
    const std::uint64_t independently_trusted_minimum_generation,
    const std::string& trusted_absolute_operator_key_pin_path,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& operator_proof,
    const std::int64_t trusted_now_ms)
{
    try {
        // Root identity and rollback floor are independently supplied by
        // a future trusted operator enrollment/revocation controller.
        if (!lower_hex(independently_trusted_root_fingerprint, 64)
            || root_fingerprint(separately_enrolled_root_public_key)
                != independently_trusted_root_fingerprint
            || independently_trusted_minimum_generation == 0)
            return Code::invalid_root_or_independent_trust;

        const auto snapshot = parse(candidate_signed_snapshot);
        if (!snapshot) return Code::invalid_snapshot_format;
        if (!verify_root(*snapshot, separately_enrolled_root_public_key))
            return Code::invalid_root_signature;

        // A prior signed ACTIVE generation becomes unacceptable when the
        // trusted rollback floor advances. If it doesn't advance, stale
        // snapshots are still replayable; this component is NOT authority.
        if (snapshot->status != "active"
            || snapshot->generation < independently_trusted_minimum_generation
            || trusted_now_ms < snapshot->not_before_ms
            || trusted_now_ms >= snapshot->not_after_ms)
            return Code::revoked_or_stale_or_outside_validity;

        // The root-signed fingerprint, rather than the candidate's proof,
        // selects the operator key. The #282 reader loads a local owner-0400
        // file and compares it with that fingerprint before #279 signature
        // verification. This result remains explicitly non-authoritative.
        const auto pin_result =
            inspect_local_goose_dialogue_responder_retry_pinned_signature(
                trusted_absolute_operator_key_pin_path,
                snapshot->key_fingerprint,
                review, proposal, scope, challenge, operator_proof,
                trusted_now_ms);
        return pin_result
                    == LocalGooseDialogueResponderRetryPinnedSignatureCode::
                        cryptographically_consistent_no_authority
            ? Code::cryptographically_consistent_no_authority
            : Code::operator_pin_or_exact_message_invalid;
    } catch (...) {
        return Code::invalid_snapshot_format;
    }
}

} // namespace gaudere_agent
