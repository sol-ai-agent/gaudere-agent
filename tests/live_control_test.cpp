#include "LiveControl.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

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

std::string temporary_directory()
{
    char pattern[] = "/tmp/gaudere-live-control-XXXXXX";
    char* path = ::mkdtemp(pattern);
    if (!path) {
        throw std::runtime_error("mkdtemp failed");
    }
    return path;
}

void test_round_trip_and_socket_permissions()
{
    const auto directory = temporary_directory();
    const auto socket_path = directory + "/control.sock";

    LiveControlMailbox mailbox;
    std::mutex wake_mutex;
    std::condition_variable wake_condition;
    bool woke = false;
    LiveControlServer server(socket_path, mailbox, [&] {
        std::lock_guard<std::mutex> lock(wake_mutex);
        woke = true;
        wake_condition.notify_all();
    });

    expect(server.start(), "server starts once");

    struct stat metadata{};
    expect(::stat(socket_path.c_str(), &metadata) == 0,
           "socket path exists after start");
    expect((metadata.st_mode & 0777) == 0600,
           "control socket permissions are exactly 0600");

    std::ostringstream output;
    std::ostringstream error;
    int client_result = -1;
    std::thread client([&] {
        client_result = run_live_control_client(
            socket_path,
            LiveControlCommand{LiveControlOperation::submit_echo,
                               "live-test", "bonjour"},
            output, error);
    });

    {
        std::unique_lock<std::mutex> lock(wake_mutex);
        expect(wake_condition.wait_for(lock, 2s, [&] { return woke; }),
               "control request wakes worker callback");
    }

    const auto pending = mailbox.take_all();
    expect(pending.size() == 1, "worker receives exactly one queued request");
    if (pending.size() == 1) {
        expect(pending.front()->command().operation == LiveControlOperation::submit_echo,
               "operation survives socket handoff");
        expect(pending.front()->command().id == "live-test",
               "task id survives socket handoff");
        expect(pending.front()->command().text == "bonjour",
               "task text survives socket handoff");
        pending.front()->complete(LiveControlReply{true, 0, "accepted\n"});
    }

    client.join();
    expect(client_result == 0, "successful live client returns worker reply code");
    expect(output.str() == "accepted\n", "successful reply body reaches stdout");
    expect(error.str().empty(), "successful reply does not use stderr");

    server.stop();
    server.join();
    expect(::access(socket_path.c_str(), F_OK) != 0,
           "socket path is removed after clean shutdown");
    ::rmdir(directory.c_str());
}

void test_mailbox_stop_releases_pending_request()
{
    LiveControlMailbox mailbox;
    auto pending = mailbox.submit(
        LiveControlCommand{LiveControlOperation::inspect_task, "task-1", {}});
    mailbox.stop();
    const auto reply = pending->wait();
    expect(!reply.ok && reply.code != 0,
           "mailbox stop completes pending request with failure");
}

void test_invalid_id_is_rejected_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{LiveControlOperation::inspect_task, "bad id", {}},
        output, error);
    expect(result != 0, "invalid task id is rejected");
    expect(error.str().find("task id") != std::string::npos,
           "invalid task id produces explicit diagnostic");
}

void test_oversized_reflection_is_rejected_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{LiveControlOperation::submit_reflection,
                           "reflect-test", std::string(4097, 'x')},
        output, error);
    expect(result != 0, "oversized reflection objective is rejected");
    expect(error.str().find("1..4096") != std::string::npos,
           "oversized reflection produces explicit bounded diagnostic");
}

void test_wake_operation_round_trip()
{
    const auto directory = temporary_directory();
    const auto socket_path = directory + "/control.sock";
    LiveControlMailbox mailbox;
    std::mutex mutex;
    std::condition_variable condition;
    bool woke = false;
    LiveControlServer server(socket_path, mailbox, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        woke = true;
        condition.notify_all();
    });
    expect(server.start(), "wake protocol server starts");

    std::ostringstream output;
    std::ostringstream error;
    int client_result = -1;
    std::thread client([&] {
        client_result = run_live_control_client(
            socket_path,
            LiveControlCommand{LiveControlOperation::accept_wake,
                               "reflection-source", {}},
            output, error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "wake command wakes the sole worker callback");
    }
    const auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "wake command crosses the bounded mailbox exactly once");
    if (pending.size() == 1) {
        expect(pending.front()->command().operation
                   == LiveControlOperation::accept_wake
                   && pending.front()->command().id == "reflection-source"
                   && pending.front()->command().text.empty(),
               "wake operation and source identity survive socket decoding");
        pending.front()->complete(LiveControlReply{true, 0, "accepted\n"});
    }
    client.join();
    expect(client_result == 0 && output.str() == "accepted\n"
               && error.str().empty(),
           "wake client receives only the worker's completed reply");
    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_invalid_wake_revocation_reason_is_rejected_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{LiveControlOperation::revoke_wake,
                           "wake", "bad\nreason"},
        output, error);
    expect(result != 0, "wake reason control byte is rejected");
    expect(error.str().find("1..1024") != std::string::npos,
           "invalid wake reason produces an explicit bounded diagnostic");
}

void test_existing_regular_file_is_never_unlinked()
{
    const auto directory = temporary_directory();
    const auto socket_path = directory + "/control.sock";
    {
        std::ofstream file(socket_path);
        file << "do-not-delete";
    }

    LiveControlMailbox mailbox;
    LiveControlServer server(socket_path, mailbox, [] {});
    try {
        static_cast<void>(server.start());
        expect(false, "regular file at socket path prevents server start");
    } catch (const std::runtime_error&) {
        // Expected.
    }

    std::ifstream file(socket_path);
    std::string contents;
    file >> contents;
    expect(contents == "do-not-delete",
           "existing non-socket path is preserved");
    std::remove(socket_path.c_str());
    ::rmdir(directory.c_str());
}

void test_idle_server_stops_without_polling_timeout()
{
    const auto directory = temporary_directory();
    const auto socket_path = directory + "/control.sock";
    LiveControlMailbox mailbox;
    LiveControlServer server(socket_path, mailbox, [] {});
    expect(server.start(), "idle server starts");
    server.stop();
    server.join();
    expect(::access(socket_path.c_str(), F_OK) != 0,
           "idle server stop wakes blocking listener and removes socket");
    ::rmdir(directory.c_str());
}

} // namespace

int main()
{
    test_round_trip_and_socket_permissions();
    test_mailbox_stop_releases_pending_request();
    test_invalid_id_is_rejected_before_connect();
    test_oversized_reflection_is_rejected_before_connect();
    test_wake_operation_round_trip();
    test_invalid_wake_revocation_reason_is_rejected_before_connect();
    test_existing_regular_file_is_never_unlinked();
    test_idle_server_stops_without_polling_timeout();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All live control tests passed\n";
    return 0;
}
