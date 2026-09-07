#include "LocalGooseRunner.hpp"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace gaudere_agent {
namespace {

constexpr const char* goose_binary = "/usr/local/bin/goose";
constexpr const char* goose_tools_binary = "/usr/local/bin/gaudere-goose-tools-mcp";

bool regular_readable_model(const std::string& path) noexcept
{
    if (path.empty() || path.front() != '/') return false;
    struct stat metadata {};
    return ::lstat(path.c_str(), &metadata) == 0
        && S_ISREG(metadata.st_mode)
        && !S_ISLNK(metadata.st_mode)
        && ::access(path.c_str(), R_OK) == 0;
}

bool safe_absolute_path(const std::string& path) noexcept
{
    if (path.empty() || path.front() != '/' || path.size() > 240) return false;
    for (const unsigned char c : path) {
        const bool ok = (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '/' || c == '.' || c == '_' || c == ':' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::vector<char*> pointers(std::vector<std::string>& values)
{
    std::vector<char*> out;
    out.reserve(values.size() + 1);
    for (auto& value : values) out.push_back(value.data());
    out.push_back(nullptr);
    return out;
}

void kill_and_reap(const pid_t pid) noexcept
{
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

} // namespace

GooseCliInvocation make_goose_cli_invocation(const LocalGooseRunRequest& request)
{
    if (!regular_readable_model(request.model_path))
        throw std::invalid_argument("local Goose model must be a readable regular absolute path");
    if (request.prompt.empty() || request.prompt.size() > 48 * 1024)
        throw std::invalid_argument("local Goose prompt is empty or oversized");
    if (request.timeout.count() <= 0 || request.max_output_bytes == 0
        || request.max_output_bytes > 64 * 1024)
        throw std::invalid_argument("local Goose run bounds are invalid");
    if (request.tools_enabled
        && (!safe_absolute_path(request.control_socket)
            || !safe_absolute_path(request.governance_path))) {
        throw std::invalid_argument(
            "typed Goose tools require safe absolute control/governance paths");
    }
    if (!request.tools_enabled
        && (!request.control_socket.empty() || !request.governance_path.empty())) {
        throw std::invalid_argument(
            "Goose tool paths are invalid when typed tools are disabled");
    }

    GooseCliInvocation invocation;
    invocation.binary = goose_binary;
    invocation.argv = {
        goose_binary,
        "run",
        "--no-profile",
        "--no-session",
        "--provider", "local",
        "--model", request.model_path
    };
    if (request.tools_enabled) {
        const std::string extension = std::string(goose_tools_binary)
            + " --socket " + request.control_socket
            + " --governance " + request.governance_path;
        invocation.argv.push_back("--with-extension");
        invocation.argv.push_back(extension);
    }
    invocation.argv.insert(invocation.argv.end(), {
        "--quiet",
        "--text", request.prompt
    });
    invocation.environment = {
        std::string("GOOSE_MODE=") + (request.tools_enabled ? "auto" : "chat"),
        "GOOSE_PROVIDER=local",
        "GOOSE_MODEL=" + request.model_path,
        "GOOSE_MAX_TURNS=32",
        "HOME=/tmp",
        "PATH=/usr/local/bin:/usr/bin"
    };
    return invocation;
}

PosixLocalGooseRunner::PosixLocalGooseRunner(CooperativePump cooperative_pump)
    : cooperative_pump_(std::move(cooperative_pump))
{
}

LocalGooseRunResult PosixLocalGooseRunner::run(const LocalGooseRunRequest& request)
{
    LocalGooseRunResult result;
    try {
        auto invocation = make_goose_cli_invocation(request);
        int output_pipe[2] = {-1, -1};
        if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
            result.detail = "cannot create Goose output pipe";
            return result;
        }

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            ::close(output_pipe[0]); ::close(output_pipe[1]);
            result.detail = "cannot initialize Goose spawn actions";
            return result;
        }
        posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0600);

        auto argv = pointers(invocation.argv);
        auto envp = pointers(invocation.environment);
        pid_t pid = -1;
        const int spawn_result = ::posix_spawn(&pid, invocation.binary.c_str(),
                                               &actions, nullptr,
                                               argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        ::close(output_pipe[1]);
        if (spawn_result != 0) {
            ::close(output_pipe[0]);
            result.detail = "cannot spawn fixed Goose binary";
            return result;
        }

        const int old_flags = ::fcntl(output_pipe[0], F_GETFL, 0);
        if (old_flags >= 0) ::fcntl(output_pipe[0], F_SETFL, old_flags | O_NONBLOCK);
        const auto deadline = std::chrono::steady_clock::now() + request.timeout;
        int child_status = 0;
        bool child_done = false;
        std::string output;
        char buffer[4096];

        for (;;) {
            for (;;) {
                const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
                if (count > 0) {
                    output.append(buffer, static_cast<std::size_t>(count));
                    if (output.size() > request.max_output_bytes) {
                        kill_and_reap(pid);
                        ::close(output_pipe[0]);
                        result.outcome = LocalGooseRunOutcome::output_too_large;
                        result.detail = "Goose output exceeds bound";
                        return result;
                    }
                    continue;
                }
                if (count == 0) break;
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                kill_and_reap(pid);
                ::close(output_pipe[0]);
                result.detail = "cannot read Goose output";
                return result;
            }

            if (cooperative_pump_) {
                try {
                    cooperative_pump_();
                } catch (const std::exception& error) {
                    kill_and_reap(pid);
                    ::close(output_pipe[0]);
                    result.detail = std::string("Gaudere typed-tool pump failed: ")
                        + error.what();
                    return result;
                } catch (...) {
                    kill_and_reap(pid);
                    ::close(output_pipe[0]);
                    result.detail = "Gaudere typed-tool pump failed";
                    return result;
                }
            }

            if (!child_done) {
                const auto waited = ::waitpid(pid, &child_status, WNOHANG);
                if (waited == pid) child_done = true;
                else if (waited < 0 && errno != EINTR) {
                    ::close(output_pipe[0]);
                    result.detail = "cannot reap Goose process";
                    return result;
                }
            }
            if (child_done) {
                char probe;
                const auto count = ::read(output_pipe[0], &probe, 1);
                if (count > 0) {
                    output.push_back(probe);
                    if (output.size() > request.max_output_bytes) {
                        ::close(output_pipe[0]);
                        result.outcome = LocalGooseRunOutcome::output_too_large;
                        result.detail = "Goose output exceeds bound";
                        return result;
                    }
                    continue;
                }
                if (count < 0 && errno == EINTR) continue;
                if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    ::close(output_pipe[0]);
                    result.detail = "cannot finish reading Goose output";
                    return result;
                }
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                kill_and_reap(pid);
                ::close(output_pipe[0]);
                result.outcome = LocalGooseRunOutcome::timed_out;
                result.detail = "Goose local inference timed out";
                return result;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        ::close(output_pipe[0]);

        if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
            result.detail = "Goose local inference exited unsuccessfully";
            return result;
        }
        while (!output.empty()
               && (output.back() == '\n' || output.back() == '\r'
                   || output.back() == ' ' || output.back() == '\t'))
            output.pop_back();
        std::size_t first = 0;
        while (first < output.size()
               && (output[first] == '\n' || output[first] == '\r'
                   || output[first] == ' ' || output[first] == '\t'))
            ++first;
        output.erase(0, first);
        if (output.empty()) {
            result.detail = "Goose produced empty output";
            return result;
        }
        result.outcome = LocalGooseRunOutcome::succeeded;
        result.output = std::move(output);
        return result;
    } catch (const std::exception& e) {
        result.detail = e.what();
        return result;
    }
}

} // namespace gaudere_agent
