#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CONTROLLER_WIRE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_HOST_CONTROLLER_WIRE_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

// Source-only, untrusted syntax for a future independently authenticated host
// controller. Neither parsing nor the caller-supplied SHA-256 proves trust.
// No controller, transport, key, issuer, ledger, enrollment or grant is wired.
enum class RetryHostControllerIntentOperation : std::uint8_t {
    inspect_identity_read_only,
    request_scoped_challenge
};

struct RetryHostControllerUntrustedIntent final {
    RetryHostControllerIntentOperation operation;
    std::string request_context_sha256;
};

// Exact canonical ASCII/LF request, no caller-selected host, public key,
// root, trusted time, epoch, nonce, consent, provider or execution fields.
[[nodiscard]] std::optional<RetryHostControllerUntrustedIntent>
parse_untrusted_retry_host_controller_intent(const std::string& wire);

// This v1 response vocabulary represents ONLY refusals. There is no success,
// approved, issued, consumed or enrolled state and no authority to execute.
enum class RetryHostControllerDenial : std::uint8_t {
    not_enrolled,
    peer_not_authorized,
    host_trust_unavailable,
    operator_required,
    clock_uncertain,
    commit_unknown
};

[[nodiscard]] std::optional<RetryHostControllerDenial>
parse_retry_host_controller_denial(const std::string& wire);

} // namespace gaudere_agent

#endif
