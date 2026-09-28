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

void test_local_goose_dialogue_v3_operations_round_trip()
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
    expect(server.start(), "V3 live control server starts");

    std::ostringstream root_output;
    std::ostringstream root_error;
    int root_result = -1;
    std::thread root_client([&] {
        root_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::submit_local_goose_dialogue_v3_root,
                "v3-root-001", "Observation système.", {},
                "system", "sol", "observation"},
            root_output, root_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "V3 root dialogue wakes the sole worker callback");
    }
    auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "V3 root dialogue crosses bounded mailbox exactly once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::submit_local_goose_dialogue_v3_root
                   && command.id == "v3-root-001"
                   && command.text == "Observation système."
                   && command.predecessor_task_id.empty()
                   && command.speaker_kind == "system"
                   && command.speaker_id == "sol"
                   && command.message_kind == "observation",
               "V3 root preserves canonical actor provenance");
        pending.front()->complete(
            LiveControlReply{true, 0, "status=pending\n"});
    }
    root_client.join();
    expect(root_result == 0
               && root_output.str() == "status=pending\n"
               && root_error.str().empty(),
           "V3 root client receives worker reply");

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
                LiveControlOperation::submit_local_goose_dialogue_v3_next,
                "v3-next-001", "Retour du système.", predecessor,
                "system", "gaudere-runtime", "feedback"},
            next_output, next_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "V3 successor dialogue wakes the sole worker callback");
    }
    pending = mailbox.take_all();
    expect(pending.size() == 1,
           "V3 successor dialogue crosses bounded mailbox exactly once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::submit_local_goose_dialogue_v3_next
                   && command.id == "v3-next-001"
                   && command.text == "Retour du système."
                   && command.predecessor_task_id == predecessor
                   && command.speaker_kind == "system"
                   && command.speaker_id == "gaudere-runtime"
                   && command.message_kind == "feedback",
               "V3 successor preserves predecessor and actor provenance");
        pending.front()->complete(
            LiveControlReply{true, 0, "status=pending\n"});
    }
    next_client.join();
    expect(next_result == 0
               && next_output.str() == "status=pending\n"
               && next_error.str().empty(),
           "V3 successor client receives worker reply");

    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_local_goose_dialogue_v3_rejects_invalid_provenance_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v3_root,
            "v3-invalid", "message", {},
            "assistant", "sol", "dialogue"},
        output, error);
    expect(result != 0,
           "V3 dialogue rejects unsupported speaker kind before connect");
    expect(error.str().find("provenance") != std::string::npos,
           "V3 provenance rejection is explicit");
}

void test_preferred_dialogue_thread_operations_round_trip()
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
    expect(server.start(), "preferred dialogue live control server starts");

    const std::string head_task =
        "cognition.local-goose-dialogue.v2:" + std::string(64, 'a');
    std::ostringstream bind_output;
    std::ostringstream bind_error;
    int bind_result = -1;
    std::thread bind_client([&] {
        bind_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::bind_local_goose_dialogue_thread_head,
                "main", {}, head_task},
            bind_output, bind_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "preferred dialogue bind wakes worker");
    }
    auto pending = mailbox.take_all();
    expect(pending.size() == 1, "preferred dialogue bind crosses mailbox");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::bind_local_goose_dialogue_thread_head
                   && command.id == "main"
                   && command.predecessor_task_id == head_task
                   && command.text.empty(),
               "preferred dialogue bind preserves alias and head Task");
        pending.front()->complete(
            LiveControlReply{true, 0, "revision=0\n"});
    }
    bind_client.join();
    expect(bind_result == 0
               && bind_output.str() == "revision=0\n"
               && bind_error.str().empty(),
           "preferred dialogue bind receives worker reply");

    {
        std::lock_guard<std::mutex> lock(mutex);
        woke = false;
    }
    std::ostringstream send_output;
    std::ostringstream send_error;
    int send_result = -1;
    std::thread send_client([&] {
        send_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next,
                "preferred-001", "Retour système.", {},
                "system", "sol", "feedback", "main", std::uint64_t{7}},
            send_output, send_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "preferred dialogue send wakes worker");
    }
    pending = mailbox.take_all();
    expect(pending.size() == 1, "preferred dialogue send crosses mailbox");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next
                   && command.id == "preferred-001"
                   && command.text == "Retour système."
                   && command.predecessor_task_id.empty()
                   && command.speaker_kind == "system"
                   && command.speaker_id == "sol"
                   && command.message_kind == "feedback"
                   && command.thread_alias == "main"
                   && command.expected_thread_revision
                   && *command.expected_thread_revision == 7,
               "preferred dialogue send preserves revision and provenance");
        pending.front()->complete(
            LiveControlReply{true, 0, "revision=8\n"});
    }
    send_client.join();
    expect(send_result == 0
               && send_output.str() == "revision=8\n"
               && send_error.str().empty(),
           "preferred dialogue send receives worker reply");

    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_preferred_dialogue_send_requires_revision_before_connect()
{
    std::ostringstream output;
    std::ostringstream error;
    const int result = run_live_control_client(
        "/tmp/does-not-matter.sock",
        LiveControlCommand{
            LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next,
            "preferred-invalid", "message", {},
            "system", "sol", "feedback", "main"},
        output, error);
    expect(result != 0,
           "preferred dialogue send rejects missing expected revision");
    expect(error.str().find("revision") != std::string::npos,
           "preferred dialogue revision rejection is explicit");
}

void test_dialogue_completion_feed_operations_round_trip()
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
    expect(server.start(), "completion feed live control server starts");

    std::ostringstream next_output;
    std::ostringstream next_error;
    int next_result = -1;
    std::thread next_client([&] {
        next_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::inspect_local_goose_dialogue_completion,
                "sol"},
            next_output, next_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "completion feed next wakes worker");
    }
    auto pending = mailbox.take_all();
    expect(pending.size() == 1, "completion feed next crosses mailbox");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::inspect_local_goose_dialogue_completion
                   && command.id == "sol"
                   && command.text.empty(),
               "completion feed next preserves consumer id");
        pending.front()->complete(
            LiveControlReply{true, 0, "pending=true\nsequence=1\n"});
    }
    next_client.join();
    expect(next_result == 0
               && next_output.str() == "pending=true\nsequence=1\n"
               && next_error.str().empty(),
           "completion feed next receives worker reply");

    {
        std::lock_guard<std::mutex> lock(mutex);
        woke = false;
    }
    std::ostringstream ack_output;
    std::ostringstream ack_error;
    int ack_result = -1;
    std::thread ack_client([&] {
        ack_result = run_live_control_client(
            socket_path,
            LiveControlCommand{
                LiveControlOperation::acknowledge_local_goose_dialogue_completion,
                "sol", "1"},
            ack_output, ack_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "completion feed ack wakes worker");
    }
    pending = mailbox.take_all();
    expect(pending.size() == 1, "completion feed ack crosses mailbox");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::acknowledge_local_goose_dialogue_completion
                   && command.id == "sol"
                   && command.text == "1",
               "completion feed ack preserves consumer and sequence");
        pending.front()->complete(
            LiveControlReply{true, 0, "result=accepted\nlast_sequence=1\n"});
    }
    ack_client.join();
    expect(ack_result == 0
               && ack_output.str() == "result=accepted\nlast_sequence=1\n"
               && ack_error.str().empty(),
           "completion feed ack receives worker reply");

    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_dialogue_completion_ack_rejects_invalid_sequence_before_connect()
{
    for (const std::string sequence : {"0", "01", "-1", "abc"}) {
        std::ostringstream output;
        std::ostringstream error;
        const int result = run_live_control_client(
            "/tmp/does-not-matter.sock",
            LiveControlCommand{
                LiveControlOperation::acknowledge_local_goose_dialogue_completion,
                "sol", sequence},
            output, error);
        expect(result != 0,
               "completion feed ack rejects invalid sequence before connect");
        expect(error.str().find("positive sequence") != std::string::npos,
               "completion feed invalid sequence rejection is explicit");
    }
}

void test_dialogue_responder_operations_round_trip()
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
    expect(server.start(), "dialogue responder live control server starts");

    LiveControlCommand create;
    create.operation =
        LiveControlOperation::create_local_goose_dialogue_responder_lease;
    create.id = "lease-001";
    create.thread_alias = "main";
    create.speaker_id = "sol";
    create.message_kind = "feedback";
    create.text = "Bounded intervention";
    create.responder_max_system_turns = 2;
    create.responder_ttl_ms = 60000;
    create.responder_min_interval_ms = 1000;

    std::ostringstream create_output;
    std::ostringstream create_error;
    int create_result = -1;
    std::thread create_client([&] {
        create_result = run_live_control_client(
            socket_path, create, create_output, create_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "responder lease create wakes worker");
    }
    auto pending = mailbox.take_all();
    expect(pending.size() == 1,
           "responder lease create crosses bounded mailbox once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::create_local_goose_dialogue_responder_lease
                   && command.id == "lease-001"
                   && command.thread_alias == "main"
                   && command.speaker_id == "sol"
                   && command.message_kind == "feedback"
                   && command.text == "Bounded intervention"
                   && command.responder_max_system_turns
                   && *command.responder_max_system_turns == 2
                   && command.responder_ttl_ms
                   && *command.responder_ttl_ms == 60000
                   && command.responder_min_interval_ms
                   && *command.responder_min_interval_ms == 1000,
               "responder lease bounds survive socket round-trip");
        pending.front()->complete(
            LiveControlReply{true, 0, "state=active\n"});
    }
    create_client.join();
    expect(create_result == 0
               && create_output.str() == "state=active\n"
               && create_error.str().empty(),
           "responder lease create receives worker reply");

    {
        std::lock_guard<std::mutex> lock(mutex);
        woke = false;
    }

    LiveControlCommand prepare;
    prepare.operation =
        LiveControlOperation::prepare_local_goose_dialogue_responder_intent;
    prepare.id = "lease-001";
    prepare.text = "Réponse système bornée.";
    prepare.message_kind = "feedback";
    prepare.responder_completion_sequence = 7;
    prepare.responder_ttl_ms = 5000;

    std::ostringstream prepare_output;
    std::ostringstream prepare_error;
    int prepare_result = -1;
    std::thread prepare_client([&] {
        prepare_result = run_live_control_client(
            socket_path, prepare, prepare_output, prepare_error);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        expect(condition.wait_for(lock, 2s, [&] { return woke; }),
               "responder intent prepare wakes worker");
    }
    pending = mailbox.take_all();
    expect(pending.size() == 1,
           "responder intent prepare crosses bounded mailbox once");
    if (pending.size() == 1) {
        const auto& command = pending.front()->command();
        expect(command.operation
                   == LiveControlOperation::prepare_local_goose_dialogue_responder_intent
                   && command.id == "lease-001"
                   && command.message_kind == "feedback"
                   && command.text == "Réponse système bornée."
                   && command.responder_completion_sequence
                   && *command.responder_completion_sequence == 7
                   && command.responder_ttl_ms
                   && *command.responder_ttl_ms == 5000,
               "responder intent evidence survives socket round-trip");
        pending.front()->complete(
            LiveControlReply{true, 0, "state=prepared\n"});
    }
    prepare_client.join();
    expect(prepare_result == 0
               && prepare_output.str() == "state=prepared\n"
               && prepare_error.str().empty(),
           "responder intent prepare receives worker reply");

    server.stop();
    server.join();
    ::rmdir(directory.c_str());
}

void test_dialogue_responder_rejects_invalid_bounds_before_connect()
{
    LiveControlCommand bad_lease;
    bad_lease.operation =
        LiveControlOperation::create_local_goose_dialogue_responder_lease;
    bad_lease.id = "lease-bad";
    bad_lease.thread_alias = "main";
    bad_lease.speaker_id = "sol";
    bad_lease.message_kind = "feedback";
    bad_lease.text = "Purpose";
    bad_lease.responder_max_system_turns = 9;
    bad_lease.responder_ttl_ms = 60000;
    bad_lease.responder_min_interval_ms = 0;

    std::ostringstream output;
    std::ostringstream error;
    const int lease_result = run_live_control_client(
        "/tmp/does-not-matter.sock", bad_lease, output, error);
    expect(lease_result != 0,
           "responder lease rejects more than eight system turns");
    expect(error.str().find("1..8") != std::string::npos,
           "responder lease bound rejection is explicit");

    LiveControlCommand leaked_field{
        LiveControlOperation::inspect_task, "task-1", {}};
    leaked_field.responder_ttl_ms = 10;
    output.str({});
    output.clear();
    error.str({});
    error.clear();
    const int leaked_result = run_live_control_client(
        "/tmp/does-not-matter.sock", leaked_field, output, error);
    expect(leaked_result != 0,
           "unrelated operation rejects responder numeric fields");
    expect(error.str().find("responder") != std::string::npos,
           "responder field scope rejection is explicit");

    LiveControlCommand bad_prepare;
    bad_prepare.operation =
        LiveControlOperation::prepare_local_goose_dialogue_responder_intent;
    bad_prepare.id = "lease-001";
    bad_prepare.message_kind = "feedback";
    bad_prepare.text = "message";
    bad_prepare.responder_completion_sequence = 0;
    bad_prepare.responder_ttl_ms = 1000;
    output.str({});
    output.clear();
    error.str({});
    error.clear();
    const int prepare_result = run_live_control_client(
        "/tmp/does-not-matter.sock", bad_prepare, output, error);
    expect(prepare_result != 0,
           "responder prepare rejects zero completion sequence");
    expect(error.str().find("responder intent prepare") != std::string::npos,
           "responder prepare rejection is explicit");
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
    test_local_goose_dialogue_v3_operations_round_trip();
    test_local_goose_dialogue_v3_rejects_invalid_provenance_before_connect();
    test_preferred_dialogue_thread_operations_round_trip();
    test_preferred_dialogue_send_requires_revision_before_connect();
    test_dialogue_completion_feed_operations_round_trip();
    test_dialogue_completion_ack_rejects_invalid_sequence_before_connect();
    test_dialogue_responder_operations_round_trip();
    test_dialogue_responder_rejects_invalid_bounds_before_connect();
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
