#include "GaudereChat.hpp"

#include "LocalGooseDialogueV3.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

bool task_inspection_io_timeout_test()
{
    int step = 0;
    auto transport = [&](const LiveControlCommand& command, const std::chrono::milliseconds) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (step == 3 && command.operation == LiveControlOperation::inspect_task) {
            return GaudereChatTransportReply{
                1, "gaudere-control: live control receive timed out\n"};
        }
        return GaudereChatTransportReply{9, "unexpected command"};
    };

    GaudereChatSession session(
        transport, "0011aabbccddeeff");
    const auto result = session.send_turn("Salut");
    return step == 3
        && result.code == GaudereChatTurnCode::timeout
        && result.task_id == new_task
        && !session.has_ambiguous_attempt()
        && result.detail.find("receive timed out") != std::string::npos;
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

bool cumulative_inspection_budget_test()
{
    int step = 0;
    int submit_count = 0;
    auto now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::milliseconds> budgets;
    auto transport = [&](const LiveControlCommand& command,
                         const std::chrono::milliseconds budget) {
        ++step;
        if (step == 1) {
            if (command.operation != LiveControlOperation::inspect_local_goose_dialogue_thread_head
                || budget != std::chrono::milliseconds{4}) {
                return GaudereChatTransportReply{9, "wrong head budget"};
            }
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            ++submit_count;
            if (command.operation != LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next
                || budget != std::chrono::milliseconds{4}) {
                return GaudereChatTransportReply{9, "wrong submit budget"};
            }
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task) {
            return GaudereChatTransportReply{9, "wrong Task inspection"};
        }
        budgets.push_back(budget);
        const auto simulated_work = std::min(budget, std::chrono::milliseconds{3});
        now += simulated_work; // fake each network call consumes its own budget
        return GaudereChatTransportReply{0, pending_task_report()};
    };
    const auto clock = [&] { return now; };
    const auto sleeper = [&](const std::chrono::milliseconds duration) {
        now += duration;
    };
    gaudere_agent::GaudereChatOptions options;
    options.poll_interval = std::chrono::milliseconds{2};
    options.timeout = std::chrono::milliseconds{12};
    options.io_timeout = std::chrono::milliseconds{4};
    GaudereChatSession session(
        transport, "0011223344556677", options, clock, sleeper);
    const auto result = session.send_turn("Salut");
    return step == 5 // head, submit, only THREE inspections
        && submit_count == 1
        && budgets == std::vector<std::chrono::milliseconds>{
            std::chrono::milliseconds{12}, std::chrono::milliseconds{7},
            std::chrono::milliseconds{2}}
        && now.time_since_epoch() == std::chrono::milliseconds{12}
        && result.code == GaudereChatTurnCode::timeout
        && result.task_id == new_task
        && result.current_revision && *result.current_revision == 3
        && !session.has_ambiguous_attempt();
}

bool overdue_success_is_not_accepted_test()
{
    int step = 0;
    int submit_count = 0;
    auto now = std::chrono::steady_clock::time_point{};
    std::string request_id;
    auto transport = [&](const LiveControlCommand& command,
                         const std::chrono::milliseconds budget) {
        ++step;
        if (step == 1) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2) {
            ++submit_count;
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task
            || budget != std::chrono::milliseconds{10}) {
            return GaudereChatTransportReply{9, "wrong Task inspection"};
        }
        now += std::chrono::milliseconds{11}; // malicious/broken fake transport
        return GaudereChatTransportReply{0, succeeded_task_report(request_id)};
    };
    auto clock = [&] { return now; };
    gaudere_agent::GaudereChatOptions options;
    options.timeout = std::chrono::milliseconds{10};
    GaudereChatSession session(
        transport, "0011223344556677", options, clock);
    const auto result = session.send_turn("Salut");
    return step == 3 && submit_count == 1
        && result.code == GaudereChatTurnCode::timeout
        && result.task_id == new_task && result.request_id == request_id
        && result.response.empty()
        && !session.has_ambiguous_attempt();
}

bool read_only_task_status_test()
{
    for (const std::string status : {
            "pending", "running", "cancel_requested", "succeeded",
            "failed", "cancelled", "manual_review"}) {
        int calls = 0;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds budget) {
            ++calls;
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task
                || budget != std::chrono::milliseconds{17}) {
                return GaudereChatTransportReply{9, "wrong read-only request"};
            }
            return GaudereChatTransportReply{
                0, "id=\"" + new_task + "\"\n"
                   "kind=\"cognition.local-goose-dialogue.v3\"\n"
                   "status=" + status + "\n"};
        };
        gaudere_agent::GaudereChatOptions options;
        options.io_timeout = std::chrono::milliseconds{17};
        GaudereChatSession session(transport, "0123456789abcdef", options);
        const auto result = session.inspect_task(new_task);
        if (!result.ok || result.task_id != new_task || result.status != status
            || !result.detail.empty() || calls != 1
            || session.has_ambiguous_attempt()) return false;
    }
    return true;
}

bool read_only_task_rejects_invalid_id_before_transport_test()
{
    int calls = 0;
    const auto transport = [&](const LiveControlCommand&,
                               const std::chrono::milliseconds) {
        ++calls;
        return GaudereChatTransportReply{0, "status=succeeded\n"};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    for (const auto& id : std::vector<std::string>{
            "", "not-a-task", root, new_task + "x",
            "cognition.local-goose-dialogue.v3:" + std::string(64, 'A'),
            "cognition.local-goose-dialogue.v3:" + std::string(63, 'a'),
            new_task + "\nstatus=succeeded", new_task + " "}) {
        const auto result = session.inspect_task(id);
        if (result.ok || !result.status.empty() || result.task_id != id
            || calls != 0 || session.has_ambiguous_attempt()) return false;
    }
    return true;
}

bool read_only_task_rejects_malformed_or_unavailable_report_test()
{
    const std::vector<GaudereChatTransportReply> invalid = {
        {0, "id=\"" + old_head + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\nstatus=succeeded\n"},
        {0, "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v2\"\nstatus=running\n"},
        {0, "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\nstatus=granted\n"},
        {0, "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"
            "status=running\nstatus=succeeded\n"},
        {0, "id=\"" + old_head + "\"\n"
            "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\nstatus=running\n"},
        {0, "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"},
        {0, "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\nstatus=\"bad\\q\"\n"},
        {1, "gaudere-control: live control receive timed out\n"}
    };
    for (const auto& raw : invalid) {
        int calls = 0;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds budget) {
            ++calls;
            if (command.operation != LiveControlOperation::inspect_task
                || budget != std::chrono::milliseconds{5000}) {
                return GaudereChatTransportReply{9, "unexpected operation"};
            }
            return raw;
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto result = session.inspect_task(new_task);
        if (result.ok || result.status != "" || result.detail.empty()
            || calls != 1 || session.has_ambiguous_attempt()) return false;
    }
    return true;
}

bool read_only_task_after_timeout_never_resubmits_test()
{
    int calls = 0;
    int submits = 0;
    int inspections = 0;
    auto now = std::chrono::steady_clock::time_point{};
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds budget) {
        ++calls;
        if (command.operation == LiveControlOperation::inspect_local_goose_dialogue_thread_head)
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        if (command.operation == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task
            || command.id != new_task) {
            return GaudereChatTransportReply{9, "unexpected operation"};
        }
        ++inspections;
        if (inspections == 1) {
            if (budget != std::chrono::milliseconds{3})
                return GaudereChatTransportReply{9, "wrong wait budget"};
            return GaudereChatTransportReply{0, pending_task_report()};
        }
        if (budget != std::chrono::milliseconds{7})
            return GaudereChatTransportReply{9, "wrong lookup budget"};
        return GaudereChatTransportReply{
            0, "id=\"" + new_task + "\"\n"
               "kind=\"cognition.local-goose-dialogue.v3\"\n"
               "status=succeeded\n"};
    };
    gaudere_agent::GaudereChatOptions options;
    options.timeout = std::chrono::milliseconds{3};
    options.poll_interval = std::chrono::milliseconds{3};
    options.io_timeout = std::chrono::milliseconds{7};
    auto clock = [&] { return now; };
    const auto sleeper = [&](const std::chrono::milliseconds duration) {
        now += duration;
    };
    GaudereChatSession session(
        transport, "0123456789abcdef", options, clock, sleeper);
    const auto pending = session.send_turn("Salut");
    if (pending.code != GaudereChatTurnCode::timeout
        || pending.task_id != new_task || calls != 3
        || session.has_ambiguous_attempt()) return false;

    const auto lookup = session.inspect_task(pending.task_id);
    return lookup.ok && lookup.task_id == new_task
        && lookup.status == "succeeded" && lookup.detail.empty()
        && calls == 4 && submits == 1 && inspections == 2
        && !session.has_ambiguous_attempt();
}

bool wait_last_committed_recovers_canonical_response_test()
{
    int calls = 0;
    int submits = 0;
    int inspections = 0;
    std::string request_id;
    auto now = std::chrono::steady_clock::time_point{};
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds budget) {
        ++calls;
        if (command.operation
                == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (command.operation
                == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task
            || command.id != new_task || budget != std::chrono::milliseconds{3}) {
            return GaudereChatTransportReply{9, "unexpected Task inspection"};
        }
        ++inspections;
        if (inspections == 1) return GaudereChatTransportReply{0, pending_task_report()};
        return GaudereChatTransportReply{0, succeeded_task_report(request_id)};
    };
    gaudere_agent::GaudereChatOptions options;
    options.timeout = std::chrono::milliseconds{3};
    options.poll_interval = std::chrono::milliseconds{3};
    auto clock = [&] { return now; };
    const auto sleeper = [&](const std::chrono::milliseconds duration) { now += duration; };
    GaudereChatSession session(
        transport, "0123456789abcdef", options, clock, sleeper);

    const auto first = session.send_turn("Salut");
    if (first.code != GaudereChatTurnCode::timeout || first.task_id != new_task
        || first.request_id != request_id || calls != 3 || submits != 1
        || inspections != 1 || session.has_ambiguous_attempt()) return false;

    const auto resumed = session.wait_last_committed();
    return resumed.code == GaudereChatTurnCode::succeeded
        && resumed.response == "Bonjour Bertrand."
        && resumed.task_id == new_task && resumed.request_id == request_id
        && resumed.current_revision && *resumed.current_revision == 3
        && now.time_since_epoch() == std::chrono::milliseconds{3}
        && calls == 4 && submits == 1 && inspections == 2
        && !session.has_ambiguous_attempt();
}

bool wait_last_without_commit_fails_closed_test()
{
    int calls = 0;
    const auto transport = [&](const LiveControlCommand&,
                               const std::chrono::milliseconds) {
        ++calls;
        return GaudereChatTransportReply{0, pending_task_report()};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    const auto result = session.wait_last_committed();
    return calls == 0 && result.code == GaudereChatTurnCode::invalid_reply
        && result.task_id.empty() && result.response.empty()
        && result.detail.find("no verified committed") != std::string::npos;
}

bool wait_last_ambiguous_not_upgraded_to_commit_test()
{
    int calls = 0;
    int submits = 0;
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds) {
        ++calls;
        if (command.operation
                == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (command.operation
                == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            return GaudereChatTransportReply{1, "gaudere-control: send timed out\n"};
        }
        return GaudereChatTransportReply{9, "unexpected inspection"};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    const auto first = session.send_turn("Salut");
    if (first.code != GaudereChatTurnCode::transport_ambiguous
        || !session.has_ambiguous_attempt() || submits != 1 || calls != 2) return false;
    const auto wait = session.wait_last_committed();
    return wait.code == GaudereChatTurnCode::invalid_reply
        && calls == 2 && submits == 1 && session.has_ambiguous_attempt()
        && session.ambiguous_message() == "Salut";
}

bool wait_last_rejects_forged_v3_response_test()
{
    int reads = 0;
    int submits = 0;
    std::string request_id;
    auto now = std::chrono::steady_clock::time_point{};
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds) {
        if (command.operation
                == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (command.operation
                == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task
            || command.id != new_task) {
            return GaudereChatTransportReply{9, "wrong read-only operation"};
        }
        ++reads;
        if (reads == 1) return GaudereChatTransportReply{0, pending_task_report()};
        return GaudereChatTransportReply{0, succeeded_task_report(request_id, "system")};
    };
    gaudere_agent::GaudereChatOptions options;
    options.timeout = std::chrono::milliseconds{2};
    options.poll_interval = std::chrono::milliseconds{2};
    const auto clock = [&] { return now; };
    const auto sleeper = [&](const std::chrono::milliseconds duration) { now += duration; };
    GaudereChatSession session(
        transport, "0123456789abcdef", options, clock, sleeper);
    const auto initial = session.send_turn("Salut");
    if (initial.code != GaudereChatTurnCode::timeout) return false;
    const auto resumed = session.wait_last_committed();
    return reads == 2 && submits == 1
        && resumed.code == GaudereChatTurnCode::invalid_reply
        && resumed.response.empty()
        && resumed.detail.find("not canonical v3") != std::string::npos
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
        {"task_inspection_io_timeout_test", task_inspection_io_timeout_test},
        {"timeout_test", timeout_test},
        {"cumulative_inspection_budget_test", cumulative_inspection_budget_test},
        {"overdue_success_is_not_accepted_test", overdue_success_is_not_accepted_test},
        {"read_only_task_status_test", read_only_task_status_test},
        {"read_only_task_rejects_invalid_id_before_transport_test",
         read_only_task_rejects_invalid_id_before_transport_test},
        {"read_only_task_rejects_malformed_or_unavailable_report_test",
         read_only_task_rejects_malformed_or_unavailable_report_test},
        {"read_only_task_after_timeout_never_resubmits_test",
         read_only_task_after_timeout_never_resubmits_test},
        {"wait_last_committed_recovers_canonical_response_test",
         wait_last_committed_recovers_canonical_response_test},
        {"wait_last_without_commit_fails_closed_test",
         wait_last_without_commit_fails_closed_test},
        {"wait_last_ambiguous_not_upgraded_to_commit_test",
         wait_last_ambiguous_not_upgraded_to_commit_test},
        {"wait_last_rejects_forged_v3_response_test",
         wait_last_rejects_forged_v3_response_test},
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
