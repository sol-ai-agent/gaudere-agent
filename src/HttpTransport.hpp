#ifndef GAUDERE_AGENT_HTTP_TRANSPORT_HPP
#define GAUDERE_AGENT_HTTP_TRANSPORT_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gaudere_agent {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    std::chrono::milliseconds timeout{0};
    std::uint64_t max_response_bytes = 0;
};

/** A sensitive header borrowed only for the duration of perform().
 *
 * Implementations must not retain, log, persist, or expose value after perform()
 * returns. The value normally points into a short-lived SecretValue owned by the
 * provider adapter.
 */
struct HttpSensitiveHeader {
    std::string_view name;
    std::string_view prefix;
    std::string_view value;
};

struct HttpResponse {
    long status = 0;
    std::vector<HttpHeader> headers;
    std::string body;
};

enum class HttpTransportOutcome {
    response,
    effect_unknown
};

struct HttpTransportResult {
    HttpTransportOutcome outcome = HttpTransportOutcome::effect_unknown;
    std::optional<HttpResponse> response;
    std::string failure_code;
    std::string failure_message;
};

/** Synchronous bounded HTTP transport boundary.
 *
 * A transport must enforce request.timeout and request.max_response_bytes. Any
 * network/transport state in which the remote side may have received the request
 * must be reported as effect_unknown. This interface makes no retry attempt.
 */
class HttpTransport {
public:
    virtual ~HttpTransport() = default;

    [[nodiscard]] virtual HttpTransportResult perform(
        const HttpRequest& request,
        std::optional<HttpSensitiveHeader> sensitive_header = std::nullopt) = 0;
};

} // namespace gaudere_agent

#endif
