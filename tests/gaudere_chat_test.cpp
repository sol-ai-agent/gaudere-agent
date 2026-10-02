#include "GaudereChat.hpp"

#include "LocalGooseDialogueV3.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using gaudere_agent::GaudereChatSession;
using gaudere_agent::GaudereChatTransportReply;
using gaudere_agent::GaudereChatTurnCode;
using gaudere_agent::LiveControlCommand;
using gaudere_agent::LiveControlOperation;
using Json = nlohmann::json;

const std::string root =
    "cognition.local-goose-dialogue.v2:" + std::string(64, 'b');
const std::string old_head =
    "cognition.local-goose-dialogue.v3:" + std::string(64, 'c');
const std::string new_task =
    "cognition.local-goose-dialogue.v3:" + std::string(64, 'd');
const std::string model_sha(64, 'e');
const std::string predecessor_sha(64, 'f');

std::string quote_report(const std::string& value)
{
    std::string out{"\""};
    for (const unsigned char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(static_cast<char>(c)); break;
        }
    }
    out.push_back('"');
    return out;
}

std::string head_report(const std::uint64_t revision,
                        const std::string& head)
{
    return "alias=\"main\"\n"
        "revision=" + std::to_string(revision) + "\n"
        "root_task_id=\"" + root + "\"\n"
        "head_task_id=\"" + head + "\"\n";
}

std::string submit_report(const std::uint64_t revision)
{
    return "gaudere-agent: preferred dialogue request committed\n"
        "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "status=pending\n"
        "attempts=0/2\n"
        + head_report(revision, new_task);
}

std::string response_json(const std::string& request_id,
                          const std::string& speaker_kind = "human")
{
    return Json{
        {"message_kind", "dialogue"},
        {"model_sha256", model_sha},
        {"predecessor_result_sha256", predecessor_sha},
        {"predecessor_task_id", old_head},
        {"request_id", request_id},
        {"response", "Bonjour Bertrand."},
        {"root_task_id", root},
        {"schema", gaudere_agent::local_goose_dialogue_v3_response_schema},
        {"speaker_id", "bertrand"},
        {"speaker_kind", speaker_kind},
        {"turn_index", 4}
    }.dump();
}

std::string succeeded_task_report(const std::string& request_id,
                                  const std::string& speaker_kind = "human")
{
    const auto raw = response_json(request_id, speaker_kind);
    return "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "status=succeeded\n"
        "attempts=1/2\n"
        "result_content_type=\"application/vnd.gaudere.local-goose-dialogue-v3-response+json\"\n"
        "result_output=" + quote_report(raw) + "\n";
}

std::string pending_task_report()
{
    return "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "status=running\n"
        "attempts=1/2\n";
}

bool check_command_common(const LiveControlCommand& command,
                          const std::string& request_id)
{
    return command.id == request_id
        && command.thread_alias == "main"
        && command.expected_thread_revision
        && *command.expected_thread_revision == 2
        && command.speaker_kind == "human"
        && command.speaker_id == "bertrand"
        && command.message_kind == "dialogue"
        && command.text == "Salut";
}

bool successful_turn_test()
{
    int step = 0;
    std::string request_id;
    auto transport = [&](const LiveControlCommand& command, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            if (command.operation
                != LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
                return GaudereChatTransportReply{9, "wrong head operation"};
            }
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            request_id = command.id;
            if (command.operation
                    != LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next
                || !check_command_common(command, request_id)) {
                return GaudereChatTransportReply{9, "wrong submit"};
            }
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (step == 3) {
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task) {
                return GaudereChatTransportReply{9, "wrong task inspect"};
            }
            return GaudereChatTransportReply{
                0, succeeded_task_report(request_id)};
        }
        return GaudereChatTransportReply{9, "unexpected command"};
    };

    GaudereChatSession session(
        transport, "0011223344556677");
    const auto result = session.send_turn("Salut");
    return step == 3
        && result.code == GaudereChatTurnCode::succeeded
        && result.request_id == request_id
        && result.task_id == new_task
        && result.response == "Bonjour Bertrand."
        && result.current_revision
        && *result.current_revision == 3
        && request_id.rfind("chat-v1-0011223344556677-1-", 0) == 0;
}

bool conflict_test()
{
    int step = 0;
    int submit_count = 0;
    auto transport = [&](const LiveControlCommand& command, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            ++submit_count;
            if (command.operation
                != LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                return GaudereChatTransportReply{9, "wrong conflict operation"};
            }
            return GaudereChatTransportReply{
                4,
                "gaudere-agent: preferred dialogue thread revision conflict\n"
                + head_report(3, "cognition.local-goose-dialogue.v3:"
                                    + std::string(64, 'a'))};
        }
        if (step == 3) {
            return GaudereChatTransportReply{
                0,
                head_report(3, "cognition.local-goose-dialogue.v3:"
                                   + std::string(64, 'a'))};
        }
        return GaudereChatTransportReply{9, "unexpected command"};
    };

    GaudereChatSession session(
        transport, "1111222233334444");
    const auto result = session.send_turn("Salut");
    return step == 3
        && submit_count == 1
        && result.code == GaudereChatTurnCode::conflict
        && result.current_revision
        && *result.current_revision == 3
        && !result.orphan_task_may_exist
        && !session.has_ambiguous_attempt();
}

bool ambiguous_exact_retry_test()
{
    int step = 0;
    std::string first_request;
    auto transport = [&](const LiveControlCommand& command, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            first_request = command.id;
            if (!check_command_common(command, first_request)) {
                return GaudereChatTransportReply{9, "wrong first submit"};
            }
            return GaudereChatTransportReply{
                1, "gaudere-control: connection reset\n"};
        }
        if (step == 3) {
            if (command.id != first_request
                || !check_command_common(command, first_request)) {
                return GaudereChatTransportReply{9, "retry changed identity"};
            }
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (step == 4) {
            return GaudereChatTransportReply{
                0, succeeded_task_report(first_request)};
        }
        return GaudereChatTransportReply{9, "unexpected command"};
    };

    GaudereChatSession session(
        transport, "aaaabbbbccccdddd");
    const auto first = session.send_turn("Salut");
    if (first.code != GaudereChatTurnCode::transport_ambiguous
        || !session.has_ambiguous_attempt()
        || session.ambiguous_message() != "Salut") {
        return false;
    }

    const auto blocked = session.send_turn("Autre");
    if (blocked.code != GaudereChatTurnCode::transport_ambiguous
        || step != 2) {
        return false;
    }

    const auto retry = session.retry_ambiguous();
    return step == 4
        && retry.code == GaudereChatTurnCode::succeeded
        && retry.request_id == first_request
        && !session.has_ambiguous_attempt();
}

bool timeout_test()
{
    int step = 0;
    auto now = std::chrono::steady_clock::time_point{};
    auto transport = [&](const LiveControlCommand&, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        return GaudereChatTransportReply{0, pending_task_report()};
    };
    auto clock = [&] { return now; };
    auto sleeper = [&](const std::chrono::milliseconds duration) {
        now += duration;
    };

    gaudere_agent::GaudereChatOptions options;
    options.poll_interval = std::chrono::milliseconds{2};
    options.timeout = std::chrono::milliseconds{3};
    GaudereChatSession session(
        transport, "0123456789abcdef", options, clock, sleeper);
    const auto result = session.send_turn("Salut");
    return step == 4
        && result.code == GaudereChatTurnCode::timeout
        && result.task_id == new_task
        && !session.has_ambiguous_attempt();
}


bool task_inspection_deadline_does_not_make_submission_ambiguous_test()
{
    int step = 0;
    std::chrono::milliseconds task_deadline{0};
    auto transport = [&](const LiveControlCommand&,
                         const std::chrono::milliseconds timeout) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        task_deadline = timeout;
        return GaudereChatTransportReply{
            gaudere_agent::live_control_client_timeout_code,
            "gaudere-control: live control response timed out\n"};
    };

    gaudere_agent::GaudereChatOptions options;
    options.transport_timeout = std::chrono::milliseconds{25};
    options.timeout = std::chrono::milliseconds{400};
    GaudereChatSession session(
        transport, "9999aaaabbbbcccc", options);
    const auto result = session.send_turn("Salut");
    return step == 3
        && task_deadline.count() > 0
        && task_deadline <= options.timeout
        && result.code == GaudereChatTurnCode::timeout
        && result.task_id == new_task
        && !session.has_ambiguous_attempt();
}

bool noncanonical_response_test()
{
    int step = 0;
    std::string request_id;
    auto transport = [&](const LiveControlCommand& command, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        return GaudereChatTransportReply{
            0, succeeded_task_report(request_id, "system")};
    };

    GaudereChatSession session(
        transport, "fedcba9876543210");
    const auto result = session.send_turn("Salut");
    return result.code == GaudereChatTurnCode::invalid_reply
        && result.detail.find("not canonical v3") != std::string::npos;
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<bool()>>> tests{
        {"successful_turn_test", successful_turn_test},
        {"conflict_test", conflict_test},
        {"ambiguous_exact_retry_test", ambiguous_exact_retry_test},
        {"timeout_test", timeout_test},
        {"task_inspection_deadline_does_not_make_submission_ambiguous_test",
         task_inspection_deadline_does_not_make_submission_ambiguous_test},
        {"noncanonical_response_test", noncanonical_response_test}
    };

    for (const auto& [name, test] : tests) {
        if (!test()) {
            std::cerr << "FAIL: " << name << '\n';
            return 1;
        }
        std::cout << "PASS: " << name << '\n';
    }
    return 0;
}
