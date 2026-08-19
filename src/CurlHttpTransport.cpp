#include "CurlHttpTransport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace gaudere_agent {
namespace {

constexpr std::uint64_t max_response_header_bytes = 64 * 1024;

void require_curl(const CURLcode code, const char* operation)
{
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string(operation) + ": "
                                 + curl_easy_strerror(code));
    }
}

class EasyHandle final {
public:
    EasyHandle() : handle_(curl_easy_init())
    {
        if (!handle_) {
            throw std::runtime_error("curl_easy_init failed");
        }
    }

    ~EasyHandle() { curl_easy_cleanup(handle_); }

    EasyHandle(const EasyHandle&) = delete;
    EasyHandle& operator=(const EasyHandle&) = delete;

    [[nodiscard]] CURL* get() const noexcept { return handle_; }

private:
    CURL* handle_;
};

class HeaderList final {
public:
    ~HeaderList() { curl_slist_free_all(list_); }

    HeaderList(const HeaderList&) = delete;
    HeaderList& operator=(const HeaderList&) = delete;
    HeaderList() = default;

    void append(const std::string& header)
    {
        curl_slist* updated = curl_slist_append(list_, header.c_str());
        if (!updated) {
            throw std::runtime_error("curl_slist_append failed");
        }
        list_ = updated;
    }

    [[nodiscard]] curl_slist* get() const noexcept { return list_; }

private:
    curl_slist* list_ = nullptr;
};

class SensitiveString final {
public:
    explicit SensitiveString(const std::string_view value)
        : value_(value)
    {
    }

    ~SensitiveString()
    {
        volatile char* data = value_.data();
        for (std::size_t index = 0; index < value_.size(); ++index) {
            data[index] = 0;
        }
    }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;

    [[nodiscard]] const char* c_str() const noexcept { return value_.c_str(); }

private:
    std::string value_;
};

bool ascii_case_equal(const std::string_view left,
                      const std::string_view right) noexcept
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

bool valid_header_name(const std::string_view name) noexcept
{
    if (name.empty()) {
        return false;
    }
    for (const unsigned char character : name) {
        if (!std::isalnum(character) && character != '-') {
            return false;
        }
    }
    return true;
}

bool valid_header_value(const std::string_view value) noexcept
{
    for (const unsigned char character : value) {
        if (character == '\r' || character == '\n' || character == 0) {
            return false;
        }
    }
    return true;
}

bool valid_bearer_value(const std::string_view value) noexcept
{
    if (value.empty()) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x21 || character > 0x7e) {
            return false;
        }
    }
    return true;
}

std::string_view trim_header_value(std::string_view value) noexcept
{
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == '\r' || value.back() == '\n'
               || value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

struct ReceiveContext {
    explicit ReceiveContext(const std::uint64_t limit)
        : max_body_bytes(limit)
    {
    }

    std::uint64_t max_body_bytes;
    std::uint64_t header_bytes = 0;
    bool body_limit_exceeded = false;
    bool header_limit_exceeded = false;
    bool callback_failure = false;
    std::string body;
    std::vector<HttpHeader> headers;
};

std::size_t write_body(char* data,
                       const std::size_t size,
                       const std::size_t count,
                       void* user_data) noexcept
{
    auto& context = *static_cast<ReceiveContext*>(user_data);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context.body_limit_exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    const std::size_t bytes = size * count;
    if (bytes > context.max_body_bytes
        || context.body.size() > context.max_body_bytes - bytes) {
        context.body_limit_exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    try {
        context.body.append(data, bytes);
    } catch (...) {
        context.callback_failure = true;
        return CURL_WRITEFUNC_ERROR;
    }
    return bytes;
}

std::size_t receive_header(char* data,
                           const std::size_t size,
                           const std::size_t count,
                           void* user_data) noexcept
{
    auto& context = *static_cast<ReceiveContext*>(user_data);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context.header_limit_exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    const std::size_t bytes = size * count;
    if (bytes > max_response_header_bytes
        || context.header_bytes > max_response_header_bytes - bytes) {
        context.header_limit_exceeded = true;
        return CURL_WRITEFUNC_ERROR;
    }
    context.header_bytes += bytes;

    try {
        std::string_view line(data, bytes);
        if (line.empty() || line == "\r\n" || line.rfind("HTTP/", 0) == 0) {
            return bytes;
        }
        const auto separator = line.find(':');
        if (separator == std::string_view::npos || separator == 0) {
            return bytes;
        }
        const auto name = line.substr(0, separator);
        const auto value = trim_header_value(line.substr(separator + 1));
        context.headers.push_back(HttpHeader{std::string(name), std::string(value)});
    } catch (...) {
        context.callback_failure = true;
        return CURL_WRITEFUNC_ERROR;
    }
    return bytes;
}

HttpTransportResult curl_failure(const CURLcode code,
                                 const ReceiveContext& receive,
                                 const std::array<char, CURL_ERROR_SIZE>& error_buffer)
{
    std::string failure_code;
    if (receive.body_limit_exceeded || receive.header_limit_exceeded) {
        failure_code = "curl_response_too_large";
    } else if (receive.callback_failure) {
        failure_code = "curl_callback_error";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        failure_code = "curl_timeout";
    } else {
        failure_code = "curl_error_" + std::to_string(static_cast<int>(code));
    }

    const std::string message = error_buffer[0] != '\0'
        ? std::string(error_buffer.data())
        : std::string(curl_easy_strerror(code));
    return HttpTransportResult{HttpTransportOutcome::effect_unknown,
                               std::nullopt,
                               std::move(failure_code),
                               message};
}

} // namespace

CurlGlobal::CurlGlobal()
{
    const auto code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string("curl_global_init failed: ")
                                 + curl_easy_strerror(code));
    }
}

CurlGlobal::~CurlGlobal()
{
    curl_global_cleanup();
}

HttpTransportResult CurlHttpTransport::perform(
    const HttpRequest& request,
    const std::optional<HttpSensitiveHeader> sensitive_header)
{
    static_cast<void>(global_);
    if (request.method != "POST") {
        throw std::invalid_argument("CurlHttpTransport currently supports POST only");
    }
    if (request.url.empty()) {
        throw std::invalid_argument("HTTP request URL must not be empty");
    }
    if (request.timeout.count() <= 0 || request.timeout.count() > LONG_MAX) {
        throw std::invalid_argument("HTTP timeout must fit a positive long millisecond value");
    }
    if (request.max_response_bytes == 0) {
        throw std::invalid_argument("HTTP response byte limit must be positive");
    }
    if (request.body.size()
        > static_cast<std::uint64_t>(std::numeric_limits<curl_off_t>::max())) {
        throw std::invalid_argument("HTTP request body is too large for libcurl");
    }

    HeaderList headers;
    for (const auto& header : request.headers) {
        if (!valid_header_name(header.name) || !valid_header_value(header.value)) {
            throw std::invalid_argument("invalid HTTP header");
        }
        if (ascii_case_equal(header.name, "Authorization")
            || ascii_case_equal(header.name, "Proxy-Authorization")) {
            throw std::invalid_argument(
                "sensitive authorization must use HttpSensitiveHeader");
        }
        headers.append(header.name + ": " + header.value);
    }
    // Avoid an implicit 100-continue round trip and keep request semantics simple.
    headers.append("Expect:");

    std::optional<SensitiveString> bearer;
    if (sensitive_header) {
        if (sensitive_header->name != "Authorization"
            || sensitive_header->prefix != "Bearer "
            || !valid_bearer_value(sensitive_header->value)) {
            throw std::invalid_argument(
                "CurlHttpTransport only accepts a valid borrowed Bearer authorization header");
        }
        bearer.emplace(sensitive_header->value);
    }

    EasyHandle easy;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    ReceiveContext receive(request.max_response_bytes);

    require_curl(curl_easy_setopt(easy.get(), CURLOPT_ERRORBUFFER,
                                  error_buffer.data()),
                 "CURLOPT_ERRORBUFFER");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_URL, request.url.c_str()),
                 "CURLOPT_URL");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_PROTOCOLS_STR, "http,https"),
                 "CURLOPT_PROTOCOLS_STR");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_PROXY, ""),
                 "CURLOPT_PROXY");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_NETRC,
                                  static_cast<long>(CURL_NETRC_IGNORED)),
                 "CURLOPT_NETRC");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_NOSIGNAL, 1L),
                 "CURLOPT_NOSIGNAL");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 0L),
                 "CURLOPT_FOLLOWLOCATION");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_MAXREDIRS, 0L),
                 "CURLOPT_MAXREDIRS");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYPEER, 1L),
                 "CURLOPT_SSL_VERIFYPEER");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_SSL_VERIFYHOST, 2L),
                 "CURLOPT_SSL_VERIFYHOST");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT_MS,
                                  static_cast<long>(request.timeout.count())),
                 "CURLOPT_TIMEOUT_MS");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_USERAGENT,
                                  "gaudere-agent/0.1.0"),
                 "CURLOPT_USERAGENT");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_POST, 1L),
                 "CURLOPT_POST");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_POSTFIELDS,
                                  request.body.data()),
                 "CURLOPT_POSTFIELDS");
    require_curl(curl_easy_setopt(
                     easy.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.body.size())),
                 "CURLOPT_POSTFIELDSIZE_LARGE");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_HTTPHEADER, headers.get()),
                 "CURLOPT_HTTPHEADER");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_HEADEROPT,
                                  static_cast<long>(CURLHEADER_SEPARATE)),
                 "CURLOPT_HEADEROPT");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, write_body),
                 "CURLOPT_WRITEFUNCTION");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &receive),
                 "CURLOPT_WRITEDATA");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_HEADERFUNCTION, receive_header),
                 "CURLOPT_HEADERFUNCTION");
    require_curl(curl_easy_setopt(easy.get(), CURLOPT_HEADERDATA, &receive),
                 "CURLOPT_HEADERDATA");

    if (bearer) {
        require_curl(curl_easy_setopt(easy.get(), CURLOPT_HTTPAUTH,
                                      static_cast<long>(CURLAUTH_BEARER)),
                     "CURLOPT_HTTPAUTH");
        require_curl(curl_easy_setopt(easy.get(), CURLOPT_XOAUTH2_BEARER,
                                      bearer->c_str()),
                     "CURLOPT_XOAUTH2_BEARER");
    }

    const CURLcode code = curl_easy_perform(easy.get());
    if (bearer) {
        static_cast<void>(curl_easy_setopt(
            easy.get(), CURLOPT_XOAUTH2_BEARER, static_cast<char*>(nullptr)));
    }
    if (code != CURLE_OK) {
        return curl_failure(code, receive, error_buffer);
    }

    long status = 0;
    const CURLcode info = curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    if (info != CURLE_OK) {
        return HttpTransportResult{
            HttpTransportOutcome::effect_unknown,
            std::nullopt,
            "curl_response_code_error",
            curl_easy_strerror(info)};
    }

    return HttpTransportResult{
        HttpTransportOutcome::response,
        HttpResponse{status, std::move(receive.headers), std::move(receive.body)},
        {}, {}};
}

} // namespace gaudere_agent
