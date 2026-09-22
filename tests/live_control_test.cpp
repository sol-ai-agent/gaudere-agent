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

void test_local_goose_dialogue_operation_round_trip()
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
    expect(server.start(), "Local Goose dialogue protocol server starts");

    std::ostringstream output;
    std::ostringstream error;
    int client_result = -1;
    std::thread client([&] {
        client_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::submit_local_goose_dialogue,
                "dialogue-001", "Bonjour Gaudere."},
            output, error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "Local Goose dialogue wakes the sole worker callback");
    }

    const auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "Local Goose dialogue crosses the bounded mailbox exactly once");
    if (pending.size() == 1) {
        expect(pending.front()->command().operation
                   == LiveControlOperation::submit_local_goose_dialogue
                   && pending.front()->command().id == "dialogue-001"
                   && pending.front()->command().text == "Bonjour Gaudere.",
               "Local Goose dialogue preserves bounded request id and message");
        pending.front()->complete(
            LiveControlReply{true, 0, "status=pending\n"});
    }

    client.join();
    expect(client_result == 0
               && output.str() == "status=pending\n"
               && error.str().empty(),
           "Local Goose dialogue client receives worker reply");
    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_local_goose_dialogue_rejects_invalid_message_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int empty = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-empty", {}},
        output, error);
    expect(empty != 0,
           "Local Goose dialogue rejects an empty message");
    expect(error.str().find("1..4096") != std::string::npos,
           "empty local dialogue rejection is explicit");

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    const int oversized = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue,
            "dialogue-large", std::string(4097, 'x')},
        output, error);
    expect(oversized != 0,
           "Local Goose dialogue rejects an oversized message");
    expect(error.str().find("1..4096") != std::string::npos,
           "oversized local dialogue rejection is explicit");
}

void test_local_goose_dialogue_v2_operations_round_trip()
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
    expect(server.start(), "Local Goose dialogue v2 protocol server starts");

    std::ostringstream root_output;
    std::ostringstream root_error;
    int root_result = -1;
    std::thread root_client([&] {
        root_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::submit_local_goose_dialogue_v2_root,
                "thread-root-001", "Bonjour V2."},
            root_output, root_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "V2 root dialogue wakes the sole worker callback");
    }
    auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "V2 root dialogue crosses bounded mailbox exactly once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::submit_local_goose_dialogue_v2_root
                   && command.id == "thread-root-001"
                   && command.text == "Bonjour V2."
                   && command.predecessor_task_id.empty(),
               "V2 root preserves request/message and carries no predecessor");
        pending.front()->complete(
            LiveControlReply{true, 0, "status=pending\n"});
    }
    root_client.join();
    expect(root_result == 0
               && root_output.str() == "status=pending\n"
               && root_error.str().empty(),
           "V2 root client receives worker reply");

    {
        std::lock_guard<std::mutex> lock(mutex);
        woke = false;
    }
    const std::string predecessor =
        "cognition.local-goose-dialogue.v2:" + std::string(64, 'a');
    std::ostringstream next_output;
    std::ostringstream next_error;
    int next_result = -1;
    std::thread next_client([&] {
        next_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::submit_local_goose_dialogue_v2_next,
                "thread-next-001", "Suite V2.", predecessor},
            next_output, next_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "V2 successor dialogue wakes the sole worker callback");
    }
    pending = mailbox.take_all();
    expect(pending.size() == 1,
           "V2 successor dialogue crosses bounded mailbox exactly once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::submit_local_goose_dialogue_v2_next
                   && command.id == "thread-next-001"
                   && command.text == "Suite V2."
                   && command.predecessor_task_id == predecessor,
               "V2 successor preserves explicit predecessor identity");
        pending.front()->complete(
            LiveControlReply{true, 0, "status=pending\n"});
    }
    next_client.join();
    expect(next_result == 0
               && next_output.str() == "status=pending\n"
               && next_error.str().empty(),
           "V2 successor client receives worker reply");

    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_local_goose_dialogue_v2_rejects_invalid_predecessor_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v2_next,
            "thread-next-invalid", "Suite.", "bad id"},
        output, error);
    expect(result != 0,
           "V2 successor rejects malformed predecessor before connect");
    expect(error.str().find("predecessor Task id") != std::string::npos,
           "V2 predecessor rejection is explicit");
}

void test_local_goose_stimulus_operation_round_trip()
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
    expect(server.start(), "Local Goose stimulus protocol server starts");

    std::ostringstream output;
    std::ostringstream error;
    int client_result = -1;
    std::thread client([&] {
        client_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::stimulate_local_goose_cycle,
                "recheck-001", {}},
            output, error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "Local Goose stimulus wakes the sole worker callback");
    }

    const auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "Local Goose stimulus crosses the bounded mailbox exactly once");
    if (pending.size() == 1) {
        expect(pending.front()->command().operation
                   == LiveControlOperation::stimulate_local_goose_cycle
                   && pending.front()->command().id == "recheck-001"
                   && pending.front()->command().text.empty(),
               "Local Goose stimulus carries only its bounded request identity");
        pending.front()->complete(
            LiveControlReply{true, 0, "result=consumed\n"});
    }

    client.join();
    expect(client_result == 0
               && output.str() == "result=consumed\n"
               && error.str().empty(),
           "Local Goose stimulus client receives worker reply");
    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_local_goose_stimulus_rejects_text_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::stimulate_local_goose_cycle,
            "recheck-002", "free form text"},
        output, error);
    expect(result != 0,
           "Local Goose stimulus rejects free-form text");
    expect(error.str().find("only a bounded request id") != std::string::npos,
           "Local Goose stimulus text rejection is explicit");
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
    test_local_goose_dialogue_operation_round_trip();
    test_local_goose_dialogue_rejects_invalid_message_before_connect();
    test_local_goose_dialogue_v2_operations_round_trip();
    test_local_goose_dialogue_v2_rejects_invalid_predecessor_before_connect();
    test_local_goose_stimulus_operation_round_trip();
    test_local_goose_stimulus_rejects_text_before_connect();
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
