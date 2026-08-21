#include "LiveControl.hpp"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_request_bytes = 24 * 1024;
constexpr std::size_t max_response_bytes = 80 * 1024;
constexpr int protocol_version = 1;

std::string operation_name(const LiveControlOperation operation)
{
    switch (operation) {
    case LiveControlOperation::submit_echo:
        return "submit_echo";
    case LiveControlOperation::submit_openai:
        return "submit_openai";
    case LiveControlOperation::submit_reflection:
        return "submit_reflection";
    case LiveControlOperation::inspect_task:
        return "inspect_task";
    case LiveControlOperation::inspect_budget:
        return "inspect_budget";
    }
    throw std::invalid_argument("unknown live control operation");
}

LiveControlOperation parse_operation(const std::string& value)
{
    if (value == "submit_echo") {
        return LiveControlOperation::submit_echo;
    }
    if (value == "submit_openai") {
        return LiveControlOperation::submit_openai;
    }
    if (value == "submit_reflection") {
        return LiveControlOperation::submit_reflection;
    }
    if (value == "inspect_task") {
        return LiveControlOperation::inspect_task;
    }
    if (value == "inspect_budget") {
        return LiveControlOperation::inspect_budget;
    }
    throw std::invalid_argument("unsupported live control operation");
}

bool safe_id(const std::string& id) noexcept
{
    if (id.empty() || id.size() > 128) {
        return false;
    }
    for (const unsigned char c : id) {
        const bool ok = (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void validate_command(const LiveControlCommand& command)
{
    if (!safe_id(command.id)) {
        throw std::invalid_argument(
            "task id must be 1..128 characters using letters, digits, '.', '_', ':', or '-'");
    }
    switch (command.operation) {
    case LiveControlOperation::submit_echo:
        if (command.text.size() > 4096) {
            throw std::invalid_argument("local.echo input exceeds 4096 bytes");
        }
        break;
    case LiveControlOperation::submit_openai:
        if (command.text.empty() || command.text.size() > 16 * 1024) {
            throw std::invalid_argument("OpenAI input must be 1..16384 bytes");
        }
        break;
    case LiveControlOperation::submit_reflection:
        if (command.text.empty() || command.text.size() > 4096) {
            throw std::invalid_argument(
                "bounded reflection objective must be 1..4096 bytes");
        }
        break;
    case LiveControlOperation::inspect_task:
        if (!command.text.empty()) {
            throw std::invalid_argument("inspect_task does not accept text");
        }
        break;
    case LiveControlOperation::inspect_budget:
        if (command.id != "openai" || !command.text.empty()) {
            throw std::invalid_argument("inspect_budget accepts only id 'openai' and no text");
        }
        break;
    }
}

std::string encode_command(const LiveControlCommand& command)
{
    validate_command(command);
    Json document = {
        {"version", protocol_version},
        {"operation", operation_name(command.operation)},
        {"id", command.id}
    };
    if (!command.text.empty()) {
        document["text"] = command.text;
    }
    return document.dump();
}

LiveControlCommand decode_command(const std::string& payload)
{
    const auto document = Json::parse(payload);
    if (!document.is_object()
        || document.value("version", 0) != protocol_version
        || !document.contains("operation") || !document.at("operation").is_string()
        || !document.contains("id") || !document.at("id").is_string()) {
        throw std::invalid_argument("invalid live control request envelope");
    }

    LiveControlCommand command;
    command.operation = parse_operation(document.at("operation").get<std::string>());
    command.id = document.at("id").get<std::string>();
    if (document.contains("text")) {
        if (!document.at("text").is_string()) {
            throw std::invalid_argument("live control text must be a string");
        }
        command.text = document.at("text").get<std::string>();
    }
    validate_command(command);
    return command;
}

std::string encode_reply(const LiveControlReply& reply)
{
    return Json{{"version", protocol_version},
                {"ok", reply.ok},
                {"code", reply.code},
                {"body", reply.body}}.dump();
}

LiveControlReply decode_reply(const std::string& payload)
{
    const auto document = Json::parse(payload);
    if (!document.is_object()
        || document.value("version", 0) != protocol_version
        || !document.contains("ok") || !document.at("ok").is_boolean()
        || !document.contains("code") || !document.at("code").is_number_integer()
        || !document.contains("body") || !document.at("body").is_string()) {
        throw std::invalid_argument("invalid live control response envelope");
    }
    return LiveControlReply{document.at("ok").get<bool>(),
                            document.at("code").get<int>(),
                            document.at("body").get<std::string>()};
}

sockaddr_un unix_address(const std::string& path)
{
    if (path.empty()) {
        throw std::invalid_argument("control socket path must not be empty");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("control socket path is too long");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

void close_fd(int& fd) noexcept
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void send_all(const int fd, const std::string& payload)
{
    std::size_t offset = 0;
    while (offset < payload.size()) {
        const auto written = ::send(fd, payload.data() + offset,
                                    payload.size() - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("live control send failed: ")
                                     + std::strerror(errno));
        }
        offset += static_cast<std::size_t>(written);
    }
}

std::string receive_until_eof(const int fd, const std::size_t limit)
{
    std::string payload;
    char buffer[4096];
    for (;;) {
        const auto count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("live control receive failed: ")
                                     + std::strerror(errno));
        }
        const auto size = static_cast<std::size_t>(count);
        if (payload.size() > limit - std::min(limit, size)) {
            throw std::runtime_error("live control message exceeds byte limit");
        }
        payload.append(buffer, size);
        if (payload.size() > limit) {
            throw std::runtime_error("live control message exceeds byte limit");
        }
    }
    return payload;
}

LiveControlReply protocol_error(const std::string& message)
{
    return LiveControlReply{false, 2, message};
}

} // namespace

PendingLiveControl::PendingLiveControl(LiveControlCommand command)
    : command_(std::move(command))
{
}

const LiveControlCommand& PendingLiveControl::command() const noexcept
{
    return command_;
}

void PendingLiveControl::complete(LiveControlReply reply)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_) {
        return;
    }
    reply_ = std::move(reply);
    completed_ = true;
    condition_.notify_all();
}

LiveControlReply PendingLiveControl::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this] { return completed_; });
    return reply_;
}

std::shared_ptr<PendingLiveControl> LiveControlMailbox::submit(LiveControlCommand command)
{
    auto pending = std::make_shared<PendingLiveControl>(std::move(command));
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
        pending->complete(LiveControlReply{false, 1, "live control is stopping"});
        return pending;
    }
    pending_.push_back(pending);
    return pending;
}

std::vector<std::shared_ptr<PendingLiveControl>> LiveControlMailbox::take_all()
{
    std::vector<std::shared_ptr<PendingLiveControl>> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(pending_.size());
    while (!pending_.empty()) {
        result.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    return result;
}

void LiveControlMailbox::stop()
{
    std::deque<std::shared_ptr<PendingLiveControl>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        pending.swap(pending_);
    }
    for (const auto& request : pending) {
        request->complete(LiveControlReply{false, 1, "live control is stopping"});
    }
}

LiveControlServer::LiveControlServer(std::string socket_path,
                                     LiveControlMailbox& mailbox,
                                     std::function<void()> wake_worker)
    : socket_path_(std::move(socket_path)),
      mailbox_(mailbox),
      wake_worker_(std::move(wake_worker))
{
    if (!wake_worker_) {
        throw std::invalid_argument("live control wake callback is required");
    }
    static_cast<void>(unix_address(socket_path_));
}

LiveControlServer::~LiveControlServer()
{
    stop();
    join();
}

bool LiveControlServer::start()
{
    if (started_) {
        return false;
    }

    struct stat existing{};
    if (::lstat(socket_path_.c_str(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            throw std::runtime_error("control socket path exists and is not a socket");
        }
        if (::unlink(socket_path_.c_str()) != 0) {
            throw std::runtime_error("cannot remove stale control socket");
        }
    } else if (errno != ENOENT) {
        throw std::runtime_error("cannot inspect control socket path");
    }

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("cannot create live control socket");
    }

    int stop_pipe[2] = {-1, -1};
    if (::pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        close_fd(listen_fd_);
        throw std::runtime_error("cannot create live control stop pipe");
    }
    stop_pipe_read_ = stop_pipe[0];
    stop_pipe_write_ = stop_pipe[1];

    const auto address = unix_address(socket_path_);
    const mode_t old_mask = ::umask(0077);
    const int bind_result = ::bind(listen_fd_,
                                   reinterpret_cast<const sockaddr*>(&address),
                                   sizeof(address));
    ::umask(old_mask);
    if (bind_result != 0 || ::chmod(socket_path_.c_str(), 0600) != 0
        || ::listen(listen_fd_, 4) != 0) {
        const int saved_errno = errno;
        close_fd(listen_fd_);
        close_fd(stop_pipe_read_);
        close_fd(stop_pipe_write_);
        ::unlink(socket_path_.c_str());
        errno = saved_errno;
        throw std::runtime_error(std::string("cannot bind live control socket: ")
                                 + std::strerror(errno));
    }

    started_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void LiveControlServer::stop()
{
    if (!started_) {
        return;
    }
    mailbox_.stop();
    if (stop_pipe_write_ >= 0) {
        const char byte = 1;
        static_cast<void>(::write(stop_pipe_write_, &byte, 1));
    }
}

void LiveControlServer::join()
{
    if (thread_.joinable()) {
        thread_.join();
    }
    if (started_) {
        close_fd(listen_fd_);
        close_fd(stop_pipe_read_);
        close_fd(stop_pipe_write_);
        ::unlink(socket_path_.c_str());
        started_ = false;
    }
}

void LiveControlServer::run()
{
    for (;;) {
        pollfd descriptors[2] = {
            {listen_fd_, POLLIN, 0},
            {stop_pipe_read_, POLLIN, 0}
        };
        int ready;
        do {
            ready = ::poll(descriptors, 2, -1);
        } while (ready < 0 && errno == EINTR);
        if (ready < 0 || (descriptors[1].revents & POLLIN) != 0) {
            return;
        }
        if ((descriptors[0].revents & POLLIN) == 0) {
            continue;
        }

        const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }

        try {
            const auto payload = receive_until_eof(client, max_request_bytes);
            const auto command = decode_command(payload);
            auto pending = mailbox_.submit(command);
            wake_worker_();
            const auto reply = pending->wait();
            const auto encoded = encode_reply(reply);
            if (encoded.size() > max_response_bytes) {
                send_all(client, encode_reply(protocol_error("live control response exceeds byte limit")));
            } else {
                send_all(client, encoded);
            }
        } catch (const std::exception& error) {
            try {
                send_all(client, encode_reply(protocol_error(error.what())));
            } catch (...) {
            }
        }
        ::close(client);
    }
}

int run_live_control_client(const std::string& socket_path,
                            const LiveControlCommand& command,
                            std::ostream& output,
                            std::ostream& error)
{
    int fd = -1;
    try {
        const auto payload = encode_command(command);
        const auto address = unix_address(socket_path);
        fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::runtime_error("cannot create live control client socket");
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error(std::string("cannot connect to live control socket: ")
                                     + std::strerror(errno));
        }
        send_all(fd, payload);
        if (::shutdown(fd, SHUT_WR) != 0) {
            throw std::runtime_error("cannot finish live control request");
        }
        const auto response = receive_until_eof(fd, max_response_bytes);
        close_fd(fd);
        const auto reply = decode_reply(response);
        if (reply.ok) {
            output << reply.body;
        } else {
            error << reply.body;
        }
        return reply.code;
    } catch (const std::exception& exception) {
        close_fd(fd);
        error << "gaudere-control: " << exception.what() << '\n';
        return 1;
    }
}

} // namespace gaudere_agent
