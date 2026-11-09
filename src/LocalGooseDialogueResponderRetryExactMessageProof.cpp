#include "LocalGooseDialogueResponderRetryExactMessageProof.hpp"

#include "LocalGooseDialogueResponder.hpp"
#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace gaudere_agent {
namespace {

using Review = LocalGooseDialogueResponderRetryExactMessageReview;
using Proposal = LocalGooseDialogueResponderRetryGrantProposal;
using Scope = LocalGooseDialogueResponderRetryScope;
using Challenge = LocalGooseDialogueResponderRetryOperatorChallenge;
using Signed = LocalGooseDialogueResponderRetrySignedExactMessageProof;
using Code = LocalGooseDialogueResponderRetryExactMessageProofCode;

constexpr const char* decision_exact = "approve_exact_retry_message_v1";
constexpr const char* signing_domain =
    "gaudere.cognition.local-goose-dialogue-responder-retry-exact-message-approval.v1|";

bool lowercase_hex(const std::string& value, const std::size_t length) noexcept
{
    if (value.size() != length) return false;
    for (const unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Canonical literal UTF-8, no silent replacement/normalization or hidden
// control/direction marks. Accept only LF as visible line separation.
// This does NOT prove what a future operator UI actually displays.
bool safe_exact_utf8(const std::string& text) noexcept
{
    if (text.empty()
        || text.size() > local_goose_dialogue_responder_retry_exact_max_message_bytes)
        return false;
    for (std::size_t pos = 0; pos < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        std::uint32_t point = 0;
        std::size_t tail = 0;
        if (lead < 0x80) {
            point = lead;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            point = lead & 0x1fu;
            tail = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            point = lead & 0x0fu;
            tail = 2;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            point = lead & 0x07u;
            tail = 3;
        } else {
            return false;
        }
        if (pos + tail >= text.size()) return false;
        for (std::size_t i = 1; i <= tail; ++i) {
            const unsigned char octet =
                static_cast<unsigned char>(text[pos + i]);
            if ((octet & 0xc0u) != 0x80u) return false;
            point = (point << 6u) | (octet & 0x3fu);
        }
        if ((tail == 1 && point < 0x80u)
            || (tail == 2 && point < 0x800u)
            || (tail == 3 && point < 0x10000u)
            || point > 0x10ffffu
            || (point >= 0xd800u && point <= 0xdfffu)
            || (point < 0x20u && point != 0x0au)
            || (point >= 0x7fu && point <= 0x9fu)
            || (point >= 0x202au && point <= 0x202eu)
            || (point >= 0x2066u && point <= 0x2069u)
            || (point >= 0x200bu && point <= 0x200du)
            || point == 0x2060u
            || (point >= 0xfdd0u && point <= 0xfdefu)
            || (point & 0xfffeu) == 0xfffeu)
            return false;
        pos += tail + 1;
    }
    return true;
}

void append(std::string& out, const std::string& field)
{
    out += std::to_string(field.size()) + ":" + field + "|";
}
void append(std::string& out, const std::uint64_t field)
{
    out += std::to_string(field) + "|";
}

bool exact_challenge(const Challenge& challenge,
                     const Proposal& proposal,
                     const Scope& scope)
{
    if (challenge.issued_at_ms < 0
        || challenge.expires_at_ms <= challenge.issued_at_ms)
        return false;
    const auto canonical =
        make_local_goose_dialogue_responder_retry_operator_challenge(
            proposal, scope, challenge.nonce_hex, challenge.issued_at_ms,
            challenge.expires_at_ms - challenge.issued_at_ms);
    return canonical
        && canonical->challenge_id == challenge.challenge_id
        && canonical->proposal_id == challenge.proposal_id
        && canonical->scope_id == challenge.scope_id
        && canonical->lineage_id == challenge.lineage_id
        && canonical->approval_reference == challenge.approval_reference
        && canonical->nonce_hex == challenge.nonce_hex
        && canonical->issued_at_ms == challenge.issued_at_ms
        && canonical->expires_at_ms == challenge.expires_at_ms;
}

std::string review_identity(const Review& r)
{
    std::string canonical =
        "gaudere.cognition.local-goose-dialogue-responder-retry-exact-message-review.v1|";
    append(canonical, r.challenge_id);
    append(canonical, r.proposal_id);
    append(canonical, r.scope_id);
    append(canonical, r.lineage_id);
    append(canonical, r.message_sha256);
    append(canonical, r.message_text);
    append(canonical, r.responder_speaker_kind);
    append(canonical, r.responder_speaker_id);
    append(canonical, static_cast<std::uint64_t>(r.retry_generation));
    append(canonical, r.completion_sequence);
    append(canonical, r.expected_head_revision);
    append(canonical, static_cast<std::uint64_t>(r.message_kind));
    return std::string(local_goose_dialogue_responder_retry_exact_review_prefix)
        + sha256_hex(canonical);
}

bool valid_review(const Review& review, const Proposal& proposal,
                  const Scope& scope, const Challenge& challenge)
{
    if (!matches_local_goose_dialogue_responder_retry_grant_scope(
            proposal, scope)
        || !exact_challenge(challenge, proposal, scope)
        || !safe_exact_utf8(review.message_text)
        || !lowercase_hex(review.message_sha256, 64)
        || review.message_sha256 != sha256_hex(review.message_text)
        || review.challenge_id != challenge.challenge_id
        || review.proposal_id != proposal.proposal_id
        || review.scope_id != scope.scope_id
        || review.lineage_id != scope.lineage_id
        || review.responder_speaker_kind != proposal.responder_speaker_kind
        || review.responder_speaker_id != proposal.responder_speaker_id
        || review.responder_speaker_kind != "system"
        || review.responder_speaker_id != "system-responder-v1"
        || review.retry_generation != 1
        || review.retry_generation != proposal.retry_generation
        || review.completion_sequence != scope.completion_sequence
        || review.expected_head_revision != scope.expected_head_revision
        || review.message_kind != local_goose_dialogue_responder_message_intervention
        || review.message_kind != proposal.allowed_message_kinds
        || review.review_id.size()
            != std::string(local_goose_dialogue_responder_retry_exact_review_prefix).size()
                + 64)
        return false;
    return review.review_id == review_identity(review);
}

std::optional<std::array<unsigned char, 64>>
parse_signature(const std::string& hex)
{
    if (!lowercase_hex(hex, 128)) return std::nullopt;
    const auto digit = [](char c) -> unsigned char {
        if (c >= '0' && c <= '9')
            return static_cast<unsigned char>(c - '0');
        return static_cast<unsigned char>(c - 'a' + 10);
    };
    std::array<unsigned char, 64> result {};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>(
            (digit(hex[2 * i]) << 4u) | digit(hex[2 * i + 1]));
    return result;
}

std::string key_fingerprint(const std::array<unsigned char, 32>& public_key)
{
    const std::string bytes(
        reinterpret_cast<const char*>(public_key.data()), public_key.size());
    return sha256_hex("gaudere-retry-operator-ed25519-key-v1|" + bytes);
}

} // namespace

std::optional<Review>
make_local_goose_dialogue_responder_retry_exact_message_review(
    const Proposal& proposal, const Scope& scope, const Challenge& challenge,
    const std::string& exact_utf8_intervention)
{
    if (!matches_local_goose_dialogue_responder_retry_grant_scope(
            proposal, scope)
        || !exact_challenge(challenge, proposal, scope)
        || !safe_exact_utf8(exact_utf8_intervention))
        return std::nullopt;
    Review review;
    review.challenge_id = challenge.challenge_id;
    review.proposal_id = proposal.proposal_id;
    review.scope_id = scope.scope_id;
    review.lineage_id = scope.lineage_id;
    review.message_sha256 = sha256_hex(exact_utf8_intervention);
    review.message_text = exact_utf8_intervention;
    review.responder_speaker_kind = proposal.responder_speaker_kind;
    review.responder_speaker_id = proposal.responder_speaker_id;
    review.retry_generation = proposal.retry_generation;
    review.completion_sequence = scope.completion_sequence;
    review.expected_head_revision = scope.expected_head_revision;
    review.message_kind = local_goose_dialogue_responder_message_intervention;
    review.review_id = review_identity(review);
    if (!valid_local_goose_dialogue_responder_retry_exact_message_review(
            review, proposal, scope, challenge))
        return std::nullopt;
    return review;
}

bool valid_local_goose_dialogue_responder_retry_exact_message_review(
    const Review& review, const Proposal& proposal,
    const Scope& scope, const Challenge& challenge) noexcept
{
    try {
        return valid_review(review, proposal, scope, challenge);
    } catch (...) {
        return false;
    }
}

std::optional<std::string>
local_goose_dialogue_responder_retry_exact_message_signing_payload(
    const Review& review, const Proposal& proposal, const Scope& scope,
    const Challenge& challenge, const std::string& operator_id,
    const std::string& decision)
{
    if (!valid_local_goose_dialogue_responder_retry_exact_message_review(
            review, proposal, scope, challenge)
        || operator_id != "bertrand" || decision != decision_exact)
        return std::nullopt;
    std::string payload = signing_domain;
    append(payload, review.review_id);
    append(payload, challenge.challenge_id);
    append(payload, proposal.proposal_id);
    append(payload, scope.scope_id);
    append(payload, scope.lineage_id);
    append(payload, scope.event_id);
    append(payload, scope.result_sha256);
    append(payload, scope.task_id);
    append(payload, challenge.approval_reference);
    append(payload, challenge.nonce_hex);
    append(payload, static_cast<std::uint64_t>(scope.retry_generation));
    append(payload, scope.completion_sequence);
    append(payload, scope.expected_head_revision);
    append(payload, static_cast<std::uint64_t>(review.message_kind));
    append(payload, review.responder_speaker_kind);
    append(payload, review.responder_speaker_id);
    append(payload, review.message_sha256);
    append(payload, review.message_text); // Raw EXACT bytes, never transformed
    append(payload, static_cast<std::uint64_t>(review.message_text.size()));
    append(payload, static_cast<std::uint64_t>(proposal.max_system_turns));
    append(payload, static_cast<std::uint64_t>(proposal.max_lease_ttl_ms));
    append(payload, challenge.issued_at_ms >= 0
        ? static_cast<std::uint64_t>(challenge.issued_at_ms) : 0u);
    append(payload, challenge.expires_at_ms >= 0
        ? static_cast<std::uint64_t>(challenge.expires_at_ms) : 0u);
    append(payload, operator_id);
    append(payload, decision);
    return payload;
}

Code inspect_local_goose_dialogue_responder_retry_exact_message_signature(
    const Review& review, const Proposal& proposal, const Scope& scope,
    const Challenge& challenge, const Signed& signed_proof,
    const std::array<unsigned char, 32>& public_key,
    const std::string& pinned_operator_id, const std::int64_t now_ms)
{
    if (!matches_local_goose_dialogue_responder_retry_grant_scope(
            proposal, scope))
        return Code::invalid_scope_or_proposal;
    if (!valid_local_goose_dialogue_responder_retry_exact_message_review(
            review, proposal, scope, challenge))
        return Code::invalid_challenge_or_message;
    if (now_ms < challenge.issued_at_ms
        || now_ms >= challenge.expires_at_ms
        || now_ms >= proposal.expires_at_ms)
        return Code::expired;
    if (pinned_operator_id != "bertrand"
        || signed_proof.operator_id != pinned_operator_id
        || signed_proof.decision != decision_exact
        || signed_proof.review_id != review.review_id
        || signed_proof.key_fingerprint != key_fingerprint(public_key))
        return Code::untrusted_key_or_actor;
    const auto signature = parse_signature(signed_proof.signature_hex);
    if (!signature)
        return Code::signature_invalid;
    const auto payload =
        local_goose_dialogue_responder_retry_exact_message_signing_payload(
            review, proposal, scope, challenge,
            signed_proof.operator_id, signed_proof.decision);
    if (!payload) return Code::invalid_challenge_or_message;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Key key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key.data(), public_key.size()),
        EVP_PKEY_free);
    if (!key) return Code::unavailable;
    Context ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) return Code::unavailable;
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
        return Code::unavailable;
    if (EVP_DigestVerify(ctx.get(), signature->data(), signature->size(),
        reinterpret_cast<const unsigned char*>(payload->data()),
        payload->size()) != 1)
        return Code::signature_invalid;

    // No key enrollment, human-visible UI, durable anti-replay, fresh
    // historical eligibility, activation, Task or ACK is provided here.
    return Code::cryptographically_consistent_no_authority;
}

} // namespace gaudere_agent
