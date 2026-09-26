#include "LiveControl.hpp"

#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

std::uint64_t parse_revision(const char* value)
{
    const std::string text{value};
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed, 10);
    if (consumed != text.size()
        || parsed > static_cast<unsigned long long>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("thread revision is invalid");
    }
    return static_cast<std::uint64_t>(parsed);
}

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program << " --socket PATH "
        << "[echo ID TEXT | openai ID TEXT | reflect ID OBJECTIVE | local-message REQUEST_ID MESSAGE | local-thread-start REQUEST_ID MESSAGE | local-thread-next REQUEST_ID PREDECESSOR_TASK_ID MESSAGE | local-thread-v3-start REQUEST_ID SPEAKER_KIND SPEAKER_ID MESSAGE_KIND MESSAGE | local-thread-v3-next REQUEST_ID PREDECESSOR_TASK_ID SPEAKER_KIND SPEAKER_ID MESSAGE_KIND MESSAGE | local-thread-v3-bind THREAD_ALIAS HEAD_TASK_ID | local-thread-v3-head THREAD_ALIAS | local-thread-v3-send REQUEST_ID THREAD_ALIAS EXPECTED_REVISION SPEAKER_KIND SPEAKER_ID MESSAGE_KIND MESSAGE | task ID | "
        << "budget | accept-wake SOURCE_TASK_ID | revoke-wake WAKE_ID REASON | "
        << "wake WAKE_ID | wake-status | stimulate-local-goose-cycle REQUEST_ID]\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc < 4 || std::string(argv[1]) != "--socket") {
            usage(argv[0]);
            return 2;
        }
        const std::string socket_path = argv[2];
        const std::string operation = argv[3];

        gaudere_agent::LiveControlCommand command;
        if (operation == "echo" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::submit_echo;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "openai" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::submit_openai;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "reflect" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::submit_reflection;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "local-message" && argc == 6) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "local-thread-start" && argc == 6) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue_v2_root;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "local-thread-next" && argc == 7) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue_v2_next;
            command.id = argv[4];
            command.predecessor_task_id = argv[5];
            command.text = argv[6];
        } else if (operation == "local-thread-v3-start" && argc == 9) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue_v3_root;
            command.id = argv[4];
            command.speaker_kind = argv[5];
            command.speaker_id = argv[6];
            command.message_kind = argv[7];
            command.text = argv[8];
        } else if (operation == "local-thread-v3-next" && argc == 10) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue_v3_next;
            command.id = argv[4];
            command.predecessor_task_id = argv[5];
            command.speaker_kind = argv[6];
            command.speaker_id = argv[7];
            command.message_kind = argv[8];
            command.text = argv[9];
        } else if (operation == "local-thread-v3-bind" && argc == 6) {
            command.operation =
                gaudere_agent::LiveControlOperation::bind_local_goose_dialogue_thread_head;
            command.id = argv[4];
            command.predecessor_task_id = argv[5];
        } else if (operation == "local-thread-v3-head" && argc == 5) {
            command.operation =
                gaudere_agent::LiveControlOperation::inspect_local_goose_dialogue_thread_head;
            command.id = argv[4];
        } else if (operation == "local-thread-v3-send" && argc == 11) {
            command.operation =
                gaudere_agent::LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next;
            command.id = argv[4];
            command.thread_alias = argv[5];
            command.expected_thread_revision = parse_revision(argv[6]);
            command.speaker_kind = argv[7];
            command.speaker_id = argv[8];
            command.message_kind = argv[9];
            command.text = argv[10];
        } else if (operation == "task" && argc == 5) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_task;
            command.id = argv[4];
        } else if (operation == "budget" && argc == 4) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_budget;
            command.id = "openai";
        } else if (operation == "accept-wake" && argc == 5) {
            command.operation = gaudere_agent::LiveControlOperation::accept_wake;
            command.id = argv[4];
        } else if (operation == "revoke-wake" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::revoke_wake;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "wake" && argc == 5) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_wake;
            command.id = argv[4];
        } else if (operation == "wake-status" && argc == 4) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_wake_status;
            command.id = "current";
        } else if (operation == "stimulate-local-goose-cycle" && argc == 5) {
            command.operation =
                gaudere_agent::LiveControlOperation::stimulate_local_goose_cycle;
            command.id = argv[4];
        } else {
            usage(argv[0]);
            return 2;
        }

        return gaudere_agent::run_live_control_client(
            socket_path, command, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "gaudere-control: " << error.what() << '\n';
        return 1;
    }
}
