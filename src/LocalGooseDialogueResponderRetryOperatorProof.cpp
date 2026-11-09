#include "LocalGooseDialogueResponderRetryOperatorProof.hpp"

#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace gaudere_agent {
namespace {

using Challenge = LocalGooseDialogueResponderRetryOperatorChallenge;
using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Scope = LocalGooseDialogueResponderRetryScope;
using Proof = LocalGooseDialogueResponderRetrySignedOperatorProof;
using Code = LocalGooseDialogueResponderRetryOperatorProofCode;
using Status = LocalGooseDialogueResponderRetryOperatorProofStatus;

constexpr const char* challenge_prefix =
    "cognition.local-goose-dialogue.responder-retry-operator-challenge.v1:";
constexpr const char* decision_exact = "approve_retry_generation_1";

Status status(const Code code, const char* detail)
{
    return {code, detail};
}

void append(std::string& out, const std::string& value)
{
    out += std::to_string(value.size()) + ":" + value + "|";
}
void append(std::string& out, const std::int64_t value)
{
    out += std::to_string(value) + "|";
}

bool lowercase_hex(const std::string& s, const std::size_t length) noexcept
{
    if (s.size() != length) return false;
    for (const unsigned char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool nonzero_nonce(const std::string& nonce) noexcept
{
    if (!lowercase_hex(nonce, 64)) return false;
    for (const auto c : nonce)
        if (c != '0') return true;
    return false;
}

std::string challenge_identity(const Challenge& challenge)
{
    std::string bytes =
        "gaudere.cognition.local-goose-dialogue-responder-retry-operator-challenge.v1|";
    append(bytes, challenge.proposal_id);
    append(bytes, challenge.scope_id);
    append(bytes, challenge.lineage_id);
    append(bytes, challenge.approval_reference);
    append(bytes, challenge.nonce_hex);
    append(bytes, challenge.issued_at_ms);
    append(bytes, challenge.expires_at_ms);
    return std::string(challenge_prefix) + sha256_hex(bytes);
}

bool valid_challenge(const Challenge& c) noexcept
{
    if (!nonzero_nonce(c.nonce_hex)
        || !lowercase_hex(c.challenge_id.substr(
            std::string(challenge_prefix).size()), 64)
        || c.challenge_id.compare(
            0, std::string(challenge_prefix).size(), challenge_prefix) != 0
        || c.proposal_id.empty()
        || c.scope_id.empty()
        || c.lineage_id.empty()
        || c.approval_reference.empty()
        || c.issued_at_ms < 0
        || c.expires_at_ms <= c.issued_at_ms
        || c.expires_at_ms - c.issued_at_ms
            > local_goose_dialogue_responder_retry_proof_max_ttl_ms)
        return false;
    try {
        return c.challenge_id == challenge_identity(c);
    } catch (...) {
        return false;
    }
}

bool matches(const Challenge& c, const Proposal& p, const Scope& s) noexcept
{
    return c.proposal_id == p.proposal_id
        && c.scope_id == s.scope_id
        && c.lineage_id == s.lineage_id
        && c.approval_reference == p.approval_reference
        && c.issued_at_ms >= p.prepared_at_ms
        && c.expires_at_ms <= p.expires_at_ms
        && matches_local_goose_dialogue_responder_retry_grant_scope(p, s);
}

std::optional<std::array<unsigned char, 64>>
read_signature(const std::string& hex)
{
    if (!lowercase_hex(hex, 128)) return std::nullopt;
    std::array<unsigned char, 64> bytes {};
    const auto nibble = [](const char c) -> unsigned char {
        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
        return static_cast<unsigned char>(c - 'a' + 10);
    };
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(
            (nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
    return bytes;
}

std::string fingerprint(const std::array<unsigned char, 32>& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.data()), key.size());
    return sha256_hex("gaudere-retry-operator-ed25519-key-v1|" + raw);
}

} // namespace

std::optional<Challenge>
make_local_goose_dialogue_responder_retry_operator_challenge(
    const Proposal& proposal,
    const Scope& scope,
    const std::string& nonce_hex,
    const std::int64_t issued_at_ms,
    const std::int64_t ttl_ms)
{
    if (!matches_local_goose_dialogue_responder_retry_grant_scope(
            proposal, scope)
        || !nonzero_nonce(nonce_hex)
        || issued_at_ms < proposal.prepared_at_ms
        || issued_at_ms >= proposal.expires_at_ms
        || ttl_ms <= 0
        || ttl_ms > local_goose_dialogue_responder_retry_proof_max_ttl_ms
        || ttl_ms > proposal.expires_at_ms - issued_at_ms)
        return std::nullopt;
    Challenge c;
    c.proposal_id = proposal.proposal_id;
    c.scope_id = scope.scope_id;
    c.lineage_id = scope.lineage_id;
    c.approval_reference = proposal.approval_reference;
    c.nonce_hex = nonce_hex;
    c.issued_at_ms = issued_at_ms;
    c.expires_at_ms = issued_at_ms + ttl_ms;
    c.challenge_id = challenge_identity(c);
    if (!valid_challenge(c) || !matches(c, proposal, scope))
        return std::nullopt;
    return c;
}

std::optional<std::string>
local_goose_dialogue_responder_retry_operator_signing_payload(
    const Challenge& c,
    const std::string& operator_id,
    const std::string& decision)
{
    if (!valid_challenge(c)
        || operator_id != "bertrand"
        || decision != decision_exact)
        return std::nullopt;
    std::string bytes =
        "gaudere.cognition.local-goose-dialogue-responder-retry-operator-approval.v1|";
    append(bytes, c.challenge_id);
    append(bytes, c.proposal_id);
    append(bytes, c.scope_id);
    append(bytes, c.lineage_id);
    append(bytes, c.approval_reference);
    append(bytes, c.nonce_hex);
    append(bytes, c.issued_at_ms);
    append(bytes, c.expires_at_ms);
    append(bytes, operator_id);
    append(bytes, decision);
    return bytes;
}

LocalGooseDialogueResponderRetryOperatorProofStatus
inspect_local_goose_dialogue_responder_retry_operator_signature(
    const Proposal& proposal,
    const Scope& scope,
    const Challenge& c,
    const Proof& signed_proof,
    const std::array<unsigned char, 32>& separately_pinned_ed25519_public_key,
    const std::string& separately_pinned_operator_id,
    const std::int64_t now_ms)
{
    if (!matches_local_goose_dialogue_responder_retry_grant_scope(
            proposal, scope))
        return status(Code::invalid_proposal_or_scope, "scope mismatch");
    if (!valid_challenge(c) || !matches(c, proposal, scope))
        return status(Code::invalid_challenge, "challenge mismatch");
    if (now_ms < c.issued_at_ms || now_ms >= c.expires_at_ms
        || now_ms >= proposal.expires_at_ms)
        return status(Code::expired, "outside exact approval window");
    if (separately_pinned_operator_id != "bertrand"
        || signed_proof.operator_id != separately_pinned_operator_id
        || signed_proof.decision != decision_exact
        || signed_proof.challenge_id != c.challenge_id
        || signed_proof.key_fingerprint
            != fingerprint(separately_pinned_ed25519_public_key))
        return status(Code::untrusted_key_or_actor, "unknown key or actor");

    const auto payload =
        local_goose_dialogue_responder_retry_operator_signing_payload(
            c, signed_proof.operator_id, signed_proof.decision);
    const auto signature = read_signature(signed_proof.signature_hex);
    if (!payload || !signature)
        return status(Code::signature_invalid, "malformed signed statement");

    using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    PublicKey key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
            separately_pinned_ed25519_public_key.data(),
            separately_pinned_ed25519_public_key.size()),
        EVP_PKEY_free);
    if (!key) return status(Code::unavailable, "verification key unavailable");
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context)
        return status(Code::unavailable, "verification context unavailable");
    if (EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr,
                             key.get()) != 1)
        return status(Code::unavailable, "signature verifier unavailable");
    const auto ok = EVP_DigestVerify(context.get(),
        signature->data(), signature->size(),
        reinterpret_cast<const unsigned char*>(payload->data()), payload->size());
    if (ok != 1)
        return status(Code::signature_invalid, "signature not valid for exact challenge");

    // A signature only proves possession of the separately pinned private key.
    // Actual human enrollment, unique nonce issuance, durable anti-replay,
    // fresh eligibility, lease and any effect require later independent gates.
    return status(Code::cryptographically_consistent_no_authority,
                  "valid signature; no human authorization or dispatch granted");
}

} // namespace gaudere_agent
