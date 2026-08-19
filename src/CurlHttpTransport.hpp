#ifndef GAUDERE_AGENT_CURL_HTTP_TRANSPORT_HPP
#define GAUDERE_AGENT_CURL_HTTP_TRANSPORT_HPP

#include "HttpTransport.hpp"

namespace gaudere_agent {

/** Process-scoped libcurl initialization guard.
 *
 * Construct this before starting auxiliary threads and keep it alive longer than
 * every CurlHttpTransport using it.
 */
class CurlGlobal final {
public:
    CurlGlobal();
    ~CurlGlobal();

    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    CurlGlobal(CurlGlobal&&) = delete;
    CurlGlobal& operator=(CurlGlobal&&) = delete;
};

/** One-shot synchronous libcurl implementation of HttpTransport.
 *
 * The first slice supports POST over HTTP/HTTPS, never follows redirects, never
 * retries internally, ignores ambient proxy/netrc configuration, verifies TLS,
 * and bounds both response headers and body.
 */
class CurlHttpTransport final : public HttpTransport {
public:
    explicit CurlHttpTransport(CurlGlobal& global) noexcept : global_(global) {}

    [[nodiscard]] HttpTransportResult perform(
        const HttpRequest& request,
        std::optional<HttpSensitiveHeader> sensitive_header = std::nullopt) override;

private:
    CurlGlobal& global_;
};

} // namespace gaudere_agent

#endif
