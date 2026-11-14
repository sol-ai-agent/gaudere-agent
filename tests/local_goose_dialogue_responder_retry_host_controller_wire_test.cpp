#include "LocalGooseDialogueResponderRetryHostControllerWire.hpp"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace gaudere_agent;
using Operation = RetryHostControllerIntentOperation;
using Denial = RetryHostControllerDenial;

const std::string sha = std::string(63, 'a') + "1";
const std::string header = "gaudere-retry-host-controller-intent.v1\n";
const std::string prefix = header + "operation=inspect_identity_read_only\n"
    + "request_context_sha256=";
const std::string valid = prefix + sha + "\n";
const std::string denial_header = "gaudere-retry-host-controller-denial.v1\nstatus=";

void valid_intents_are_only_untrusted_syntax()
{
    const auto identity = parse_untrusted_retry_host_controller_intent(valid);
    assert(identity.has_value());
    assert(identity->operation == Operation::inspect_identity_read_only);
    assert(identity->request_context_sha256 == sha);
    const auto challenge = parse_untrusted_retry_host_controller_intent(
        header + "operation=request_scoped_challenge\nrequest_context_sha256=" + sha + "\n");
    assert(challenge.has_value());
    assert(challenge->operation == Operation::request_scoped_challenge);
    assert(challenge->request_context_sha256 == sha);
}

void rejected_host_and_operator_claims()
{
    for (const auto& injected : std::vector<std::string>{
             "host_id=bertrand-fedora-01\n", "root_path=/etc/gaudere\n",
             "signer_public_key=" + sha + "\n", "root_fingerprint=" + sha + "\n",
             "uid=0\n", "namespace_uid=0\n", "trusted_now_ms=123456\n",
             "anchor_epoch=1\n", "challenge_nonce=" + sha + "\n",
             "operator_confirmed=true\n", "approval=true\n", "provider=true\n",
             "task=execute\n", "ack=true\n"}) {
        assert(!parse_untrusted_retry_host_controller_intent(valid + injected));
        assert(!parse_untrusted_retry_host_controller_intent(
            header + injected + "operation=inspect_identity_read_only\n"
            "request_context_sha256=" + sha + "\n"));
    }
}

void malformed_or_ambiguous_wire_is_rejected()
{
    const std::vector<std::string> invalid = {
        "", valid.substr(0, valid.size() - 1), valid + "\n", " " + valid,
        valid + "status=approved\n", "gaudere-retry-host-controller-intent.v2\n"
            "operation=inspect_identity_read_only\nrequest_context_sha256=" + sha + "\n",
        header + "operation=approve\nrequest_context_sha256=" + sha + "\n",
        header + "operation=issue_challenge\nrequest_context_sha256=" + sha + "\n",
        header + "request_context_sha256=" + sha + "\n"
            "operation=inspect_identity_read_only\n",
        prefix + std::string(64, '0') + "\n", prefix + std::string(64, 'a') + "\r\n",
        prefix + std::string(64, 'A') + "\n", prefix + std::string(63, 'a') + "\n",
        prefix + std::string(65, 'a') + "\n",
        prefix + sha + "\nrequest_context_sha256=" + sha + "\n",
        prefix + sha + " \n", prefix + sha + "\n\n"
    };
    for (const auto& wire : invalid)
        assert(!parse_untrusted_retry_host_controller_intent(wire));
    auto nul = valid;
    nul.insert(nul.begin() + 12, '\0');
    assert(!parse_untrusted_retry_host_controller_intent(nul));
    assert(!parse_untrusted_retry_host_controller_intent(std::string(4096, 'a')));
    // Perturb any byte other than the deliberately variable SHA-256 field.
    for (std::size_t i = 0; i < valid.size(); ++i) {
        if (i >= prefix.size() && i < valid.size() - 1) continue;
        std::string changed = valid;
        changed[i] = changed[i] == '\n' ? '\r' : 'X';
        assert(!parse_untrusted_retry_host_controller_intent(changed));
    }
}

void only_fail_closed_denials_have_wire_codes()
{
    const std::vector<std::pair<std::string, Denial>> allowed = {
        {"not_enrolled", Denial::not_enrolled},
        {"peer_not_authorized", Denial::peer_not_authorized},
        {"host_trust_unavailable", Denial::host_trust_unavailable},
        {"operator_required", Denial::operator_required},
        {"clock_uncertain", Denial::clock_uncertain},
        {"commit_unknown", Denial::commit_unknown}
    };
    for (const auto& item : allowed) {
        const auto code = parse_retry_host_controller_denial(denial_header + item.first + "\n");
        assert(code.has_value() && *code == item.second);
    }
    for (const auto& unauthorized : {
             "approved", "authenticated", "enrolled", "accepted", "consumed",
             "success", "host_identity_verified", "execute", "grant",
             "task_accepted", "provider_enabled"}) {
        assert(!parse_retry_host_controller_denial(denial_header + unauthorized + "\n"));
    }
    assert(!parse_retry_host_controller_denial(denial_header + "not_enrolled\nstatus=approved\n"));
    assert(!parse_retry_host_controller_denial(denial_header + "not_enrolled"));
    assert(!parse_retry_host_controller_denial(denial_header + "not_enrolled\r\n"));
    assert(!parse_retry_host_controller_denial("status=not_enrolled\n"));
    assert(!parse_retry_host_controller_denial(
        "gaudere-retry-host-controller-denial.v2\nstatus=not_enrolled\n"));
    assert(!parse_retry_host_controller_denial(std::string(4096, 'x')));
}

} // namespace

int main()
{
    valid_intents_are_only_untrusted_syntax();
    rejected_host_and_operator_claims();
    malformed_or_ambiguous_wire_is_rejected();
    only_fail_closed_denials_have_wire_codes();
}
