#include "CurlHttpTransport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

using namespace gaudere_agent;
using namespace std::chrono_literals;

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Exception, typename Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        std::forward<Function>(function)();
        expect(false, message);
    } catch (const Exception&) {
        // Expected.
    } catch (...) {
        expect(false, message + " (wrong exception type)");
    }
}

std::string lower_ascii(std::string value)
{
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return value;
}

std::size_t content_length(const std::string_view headers)
{
    std::size_t start = 0;
    while (start < headers.size()) {
        const auto end = headers.find("\r\n", start);
        const auto line = headers.substr(
            start, end == std::string_view::npos ? headers.size() - start : end - start);
        const std::string lowered = lower_ascii(std::string(line));
        constexpr std::string_view prefix = "content-length:";
        if (lowered.rfind(prefix, 0) == 0) {
            std::string value(line.substr(prefix.size()));
            const auto first = value.find_first_not_of(" \t");
            if (first == std::string::npos) {
                return 0;
            }
            return static_cast<std::size_t>(std::stoull(value.substr(first)));
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 2;
    }
    return 0;
}

void send_all(const int socket, const std::string_view data) noexcept
{
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::send(socket, data.data() + offset,
                                  data.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) {
            return;
        }
        offset += static_cast<std::size_t>(count);
    }
}

class LoopbackServer final {
public:
    using Responder = std::function<void(int, const std::string&)>;

    explicit LoopbackServer(Responder responder)
        : responder_(std::move(responder))
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) {
            throw std::runtime_error("socket failed");
        }

        const int enabled = 1;
        static_cast<void>(::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                                      &enabled, sizeof(enabled)));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) != 0) {
            const std::string message = std::strerror(errno);
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("bind failed: " + message);
        }
        if (::listen(listener_, 1) != 0) {
            const std::string message = std::strerror(errno);
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("listen failed: " + message);
        }

        socklen_t size = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
            const std::string message = std::strerror(errno);
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error("getsockname failed: " + message);
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve_one(); });
    }

    ~LoopbackServer()
    {
        wait();
        if (listener_ >= 0) {
            ::close(listener_);
        }
    }

    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    void wait()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::string url(const std::string_view path = "/") const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
    }

    [[nodiscard]] const std::string& request() const noexcept { return request_; }

private:
    void serve_one() noexcept
    {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client = ::accept4(listener_, reinterpret_cast<sockaddr*>(&peer),
                                     &peer_size, SOCK_CLOEXEC);
        if (client < 0) {
            return;
        }

        timeval timeout{};
        timeout.tv_sec = 2;
        static_cast<void>(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                                      &timeout, sizeof(timeout)));

        try {
            std::string data;
            char buffer[4096];
            std::size_t expected_size = 0;
            for (;;) {
                const auto count = ::recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) {
                    break;
                }
                data.append(buffer, static_cast<std::size_t>(count));
                const auto header_end = data.find("\r\n\r\n");
                if (header_end != std::string::npos && expected_size == 0) {
                    const auto body_size = content_length(
                        std::string_view(data).substr(0, header_end + 2));
                    expected_size = header_end + 4 + body_size;
                }
                if (expected_size != 0 && data.size() >= expected_size) {
                    break;
                }
            }
            request_ = std::move(data);
            responder_(client, request_);
        } catch (...) {
            // The client-side assertions report any resulting transport failure.
        }
        ::close(client);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    Responder responder_;
    std::thread worker_;
    std::string request_;
};

HttpRequest post(const std::string& url,
                 std::string body = "hello",
                 const std::uint64_t max_response_bytes = 4096,
                 const std::chrono::milliseconds timeout = 1s)
{
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = {
        {"Content-Type", "text/plain"},
        {"X-Gaudere-Test", "loopback"}
    };
    request.body = std::move(body);
    request.timeout = timeout;
    request.max_response_bytes = max_response_bytes;
    return request;
}

std::optional<std::string> response_header(const HttpResponse& response,
                                           const std::string_view name)
{
    for (const auto& header : response.headers) {
        if (lower_ascii(header.name) == lower_ascii(std::string(name))) {
            return header.value;
        }
    }
    return std::nullopt;
}

void test_post_and_bearer(CurlGlobal& global)
{
    LoopbackServer server([](const int client, const std::string&) {
        send_all(client,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain\r\n"
                 "X-Loopback: yes\r\n"
                 "Content-Length: 5\r\n"
                 "Connection: close\r\n\r\nhello");
    });

    CurlHttpTransport transport(global);
    const auto result = transport.perform(
        post(server.url("/responses"), "request-body"),
        HttpSensitiveHeader{"Authorization", "Bearer ", "synthetic-token"});
    server.wait();

    expect(result.outcome == HttpTransportOutcome::response && result.response,
           "loopback POST returns an HTTP response");
    expect(result.response && result.response->status == 200
               && result.response->body == "hello",
           "status and bounded body are captured");
    expect(result.response
               && response_header(*result.response, "X-Loopback") == "yes",
           "response headers are captured");

    const auto& raw = server.request();
    expect(raw.find("POST /responses HTTP/") != std::string::npos,
           "server receives POST path");
    expect(raw.find("X-Gaudere-Test: loopback") != std::string::npos,
           "ordinary custom header reaches server");
    expect(raw.find("Authorization: Bearer synthetic-token") != std::string::npos,
           "borrowed bearer authorization reaches server");
    expect(raw.find("\r\n\r\nrequest-body") != std::string::npos,
           "request body reaches server exactly");
}

void test_redirect_is_not_followed(CurlGlobal& global)
{
    LoopbackServer server([](const int client, const std::string&) {
        send_all(client,
                 "HTTP/1.1 302 Found\r\n"
                 "Location: http://127.0.0.1:1/forbidden\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n\r\n");
    });

    CurlHttpTransport transport(global);
    const auto result = transport.perform(post(server.url("/redirect")));
    expect(result.outcome == HttpTransportOutcome::response && result.response
               && result.response->status == 302,
           "redirect is returned as the original definite response and never followed");
}

void test_timeout_is_unknown(CurlGlobal& global)
{
    LoopbackServer server([](const int client, const std::string&) {
        std::this_thread::sleep_for(150ms);
        send_all(client,
                 "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                 "Connection: close\r\n\r\nok");
    });

    CurlHttpTransport transport(global);
    const auto result = transport.perform(
        post(server.url("/slow"), "hello", 4096, 40ms));
    expect(result.outcome == HttpTransportOutcome::effect_unknown
               && result.failure_code == "curl_timeout",
           "post-send timeout is conservatively effect_unknown");
}

void test_response_body_limit(CurlGlobal& global)
{
    LoopbackServer server([](const int client, const std::string&) {
        send_all(client,
                 "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n"
                 "Connection: close\r\n\r\n0123456789");
    });

    CurlHttpTransport transport(global);
    const auto result = transport.perform(
        post(server.url("/large"), "hello", 4));
    expect(result.outcome == HttpTransportOutcome::effect_unknown
               && result.failure_code == "curl_response_too_large",
           "response larger than bound aborts transfer conservatively");
}

void test_proxy_environment_is_ignored(CurlGlobal& global)
{
    const char* old_all_proxy = std::getenv("ALL_PROXY");
    const char* old_no_proxy = std::getenv("NO_PROXY");
    const std::optional<std::string> saved_all = old_all_proxy
        ? std::optional<std::string>{old_all_proxy} : std::nullopt;
    const std::optional<std::string> saved_no = old_no_proxy
        ? std::optional<std::string>{old_no_proxy} : std::nullopt;

    ::setenv("ALL_PROXY", "http://127.0.0.1:1", 1);
    ::setenv("NO_PROXY", "", 1);

    LoopbackServer server([](const int client, const std::string&) {
        send_all(client,
                 "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                 "Connection: close\r\n\r\nok");
    });
    CurlHttpTransport transport(global);
    const auto result = transport.perform(post(server.url("/direct")));
    expect(result.outcome == HttpTransportOutcome::response && result.response
               && result.response->status == 200,
           "ambient proxy configuration cannot divert the transport");

    if (saved_all) {
        ::setenv("ALL_PROXY", saved_all->c_str(), 1);
    } else {
        ::unsetenv("ALL_PROXY");
    }
    if (saved_no) {
        ::setenv("NO_PROXY", saved_no->c_str(), 1);
    } else {
        ::unsetenv("NO_PROXY");
    }
}

void test_protocol_and_header_validation(CurlGlobal& global)
{
    CurlHttpTransport transport(global);

    const auto protocol = transport.perform(post("file:///etc/passwd"));
    expect(protocol.outcome == HttpTransportOutcome::effect_unknown,
           "non-HTTP protocol is rejected by libcurl protocol restriction");

    auto authorization = post("http://127.0.0.1:1/");
    authorization.headers.push_back({"Authorization", "Bearer copied-secret"});
    expect_throw<std::invalid_argument>(
        [&] { static_cast<void>(transport.perform(authorization)); },
        "ordinary authorization header is rejected before networking");

    auto injected = post("http://127.0.0.1:1/");
    injected.headers.push_back({"X-Test", "value\r\nInjected: yes"});
    expect_throw<std::invalid_argument>(
        [&] { static_cast<void>(transport.perform(injected)); },
        "header newline injection is rejected before networking");
}

} // namespace

int main()
{
    // libcurl global initialization is deliberately completed before any test
    // starts the loopback server thread.
    CurlGlobal global;

    test_post_and_bearer(global);
    test_redirect_is_not_followed(global);
    test_timeout_is_unknown(global);
    test_response_body_limit(global);
    test_proxy_environment_is_ignored(global);
    test_protocol_and_header_validation(global);

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All curl HTTP transport tests passed\n";
    return 0;
}
