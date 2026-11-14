#include "LocalGooseDialogueResponderRetryHostControllerWire.hpp"

#include <string_view>

namespace gaudere_agent {
namespace {

constexpr std::string_view intent_header =
    "gaudere-retry-host-controller-intent.v1\n";
constexpr std::string_view operation_prefix = "operation=";
constexpr std::string_view request_prefix = "request_context_sha256=";
constexpr std::string_view denial_header =
    "gaudere-retry-host-controller-denial.v1\nstatus=";

bool nonzero_lower_sha256(std::string_view value) noexcept
{
    if (value.size() != 64) return false;
    bool nonzero = false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        if (c != '0') nonzero = true;
    }
    return nonzero;
}

bool take_prefix(std::string_view& wire, std::string_view expected) noexcept
{
    if (wire.substr(0, expected.size()) != expected) return false;
    wire.remove_prefix(expected.size());
    return true;
}

} // namespace

std::optional<RetryHostControllerUntrustedIntent>
parse_untrusted_retry_host_controller_intent(const std::string& text)
{
    if (text.size() > 256) return std::nullopt;
    std::string_view wire(text);
    if (!take_prefix(wire, intent_header) ||
        !take_prefix(wire, operation_prefix)) return std::nullopt;
    const auto end_operation = wire.find('\n');
    if (end_operation == std::string_view::npos) return std::nullopt;
    const auto operation = wire.substr(0, end_operation);
    RetryHostControllerIntentOperation parsed_operation;
    if (operation == "inspect_identity_read_only")
        parsed_operation = RetryHostControllerIntentOperation::inspect_identity_read_only;
    else if (operation == "request_scoped_challenge")
        parsed_operation = RetryHostControllerIntentOperation::request_scoped_challenge;
    else return std::nullopt;
    wire.remove_prefix(end_operation + 1);
    if (!take_prefix(wire, request_prefix) || wire.size() != 65 ||
        wire.back() != '\n') return std::nullopt;
    wire.remove_suffix(1);
    if (!nonzero_lower_sha256(wire)) return std::nullopt;
    return RetryHostControllerUntrustedIntent{parsed_operation, std::string(wire)};
}

std::optional<RetryHostControllerDenial>
parse_retry_host_controller_denial(const std::string& text)
{
    if (text.size() > 128) return std::nullopt;
    std::string_view wire(text);
    if (!take_prefix(wire, denial_header) || wire.empty() ||
        wire.back() != '\n') return std::nullopt;
    wire.remove_suffix(1);
    if (wire == "not_enrolled") return RetryHostControllerDenial::not_enrolled;
    if (wire == "peer_not_authorized") return RetryHostControllerDenial::peer_not_authorized;
    if (wire == "host_trust_unavailable") return RetryHostControllerDenial::host_trust_unavailable;
    if (wire == "operator_required") return RetryHostControllerDenial::operator_required;
    if (wire == "clock_uncertain") return RetryHostControllerDenial::clock_uncertain;
    if (wire == "commit_unknown") return RetryHostControllerDenial::commit_unknown;
    return std::nullopt;
}

} // namespace gaudere_agent
