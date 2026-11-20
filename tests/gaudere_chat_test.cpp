#include "GaudereChat.hpp"
#include "GaudereChatInput.hpp"

#include "LocalGooseDialogueV3.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
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
        && session.last_verified_response()
        && *session.last_verified_response() == "Bonjour Bertrand."
        && session.last_verified_committed_task_id()
        && *session.last_verified_committed_task_id() == new_task
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

bool v2_task_id_in_conflict_is_not_v3_orphan_test()
{
    int heads = 0;
    int submits = 0;
    const auto v2_task = "cognition.local-goose-dialogue.v2:"
        + std::string(64, 'd');
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds) {
        if (command.operation ==
            LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            ++heads;
            return GaudereChatTransportReply{
                0, head_report(heads == 1 ? 2 : 3, old_head)};
        }
        if (command.operation ==
            LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            return GaudereChatTransportReply{
                4, "id=\"" + v2_task + "\"\n"};
        }
        return GaudereChatTransportReply{9, "unexpected I/O"};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    const auto result = session.send_turn("Salut");
    return heads == 2 && submits == 1
        && result.code == GaudereChatTurnCode::conflict
        && !result.orphan_task_may_exist
        && result.task_id.empty()
        && !session.has_ambiguous_attempt()
        && !session.last_verified_committed_task_id();
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

bool thrown_submit_transport_preserves_exact_retry_test()
{
    // A transport callback can throw after a remote durable mutation.
    // Catch both std::exception and an unknown exception type. A repeated
    // exception must also preserve the same request/predecessor identity.
    for (const int variant : {0, 1}) {
        int heads = 0;
        int submits = 0;
        int inspections = 0;
        std::string original_request;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
                ++heads;
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            }
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submits;
                if (submits == 1) original_request = command.id;
                if (!check_command_common(command, original_request)) {
                    return GaudereChatTransportReply{9, "retry changed payload"};
                }
                if (submits == 1) {
                    if (variant == 0) {
                        throw std::runtime_error("transport failed after send");
                    }
                    throw 42;
                }
                if (variant == 1 && submits == 2) {
                    throw std::runtime_error("repeated post-send exception");
                }
                return GaudereChatTransportReply{0, submit_report(3)};
            }
            ++inspections;
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task) {
                return GaudereChatTransportReply{9, "unexpected Task inspection"};
            }
            return GaudereChatTransportReply{
                0, succeeded_task_report(original_request)};
        };

        GaudereChatSession session(transport, "0011223344556677");
        const auto first = session.send_turn("Salut");
        const auto blocked = session.send_turn("Autre");
        const auto wait = session.wait_last_committed();
        if (first.code != GaudereChatTurnCode::transport_ambiguous
            || first.request_id != original_request
            || first.detail.find("threw") == std::string::npos
            || !first.task_id.empty() || first.current_revision
            || !first.response.empty()
            || !session.has_ambiguous_attempt()
            || session.ambiguous_message() != "Salut"
            || blocked.code != GaudereChatTurnCode::transport_ambiguous
            || blocked.request_id != original_request
            || wait.code != GaudereChatTurnCode::invalid_reply
            || !wait.task_id.empty()
            || session.last_verified_committed_task_id()
            || session.last_verified_response()
            || heads != 1 || submits != 1 || inspections != 0) {
            return false;
        }

        if (variant == 1) {
            const auto again = session.retry_ambiguous();
            if (again.code != GaudereChatTurnCode::transport_ambiguous
                || again.request_id != original_request
                || again.detail.find("repeated post-send exception")
                    == std::string::npos
                || !session.has_ambiguous_attempt()
                || heads != 1 || submits != 2 || inspections != 0) {
                return false;
            }
        }

        const auto recovered = session.retry_ambiguous();
        if (recovered.code != GaudereChatTurnCode::succeeded
            || recovered.request_id != original_request
            || recovered.task_id != new_task
            || recovered.response != "Bonjour Bertrand."
            || session.has_ambiguous_attempt()
            || session.last_verified_committed_task_id() != new_task
            || session.last_verified_response() != "Bonjour Bertrand."
            || heads != 1 || submits != (variant == 1 ? 3 : 2)
            || inspections != 1) {
            return false;
        }
    }
    return true;
}

bool submit_protocol_error_retains_exact_retry_test()
{
    // A code-2 error can be emitted after the server changes durable state:
    // response-too-large / protocol framing is not a reliable NO-COMMIT proof.
    // Codes without an explicit rejection contract fail closed as well.
    for (const int first_code : {1, 2, 3, 9}) {
        int heads = 0;
        int submits = 0;
        int inspections = 0;
        std::string original_request;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
                ++heads;
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            }
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submits;
                if (submits == 1) {
                    original_request = command.id;
                    if (!check_command_common(command, original_request))
                        return GaudereChatTransportReply{9, "bad original request"};
                    return GaudereChatTransportReply{
                        first_code, "protocol error after possible Task persistence"};
                }
                if (command.id != original_request
                    || !check_command_common(command, original_request))
                    return GaudereChatTransportReply{9, "retry changed identity"};
                if (first_code == 2 && submits == 2)
                    return GaudereChatTransportReply{
                        2, "protocol error repeated after possible commit"};
                return GaudereChatTransportReply{0, submit_report(3)};
            }
            ++inspections;
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task)
                return GaudereChatTransportReply{9, "unexpected Task inspection"};
            return GaudereChatTransportReply{
                0, succeeded_task_report(original_request)};
        };

        GaudereChatSession session(transport, "0011223344556677");
        const auto first = session.send_turn("Salut");
        const auto blocked = session.send_turn("Autre");
        const auto wait = session.wait_last_committed();
        if (first.code != GaudereChatTurnCode::transport_ambiguous
            || first.request_id != original_request || !first.task_id.empty()
            || !session.has_ambiguous_attempt()
            || session.ambiguous_message() != "Salut"
            || blocked.code != GaudereChatTurnCode::transport_ambiguous
            || blocked.request_id != original_request
            || wait.code != GaudereChatTurnCode::invalid_reply
            || !wait.task_id.empty()
            || heads != 1 || submits != 1 || inspections != 0
            || session.last_verified_committed_task_id()
            || session.last_verified_response())
            return false;

        if (first_code == 2) {
            const auto repeated = session.retry_ambiguous();
            if (repeated.code != GaudereChatTurnCode::transport_ambiguous
                || repeated.request_id != original_request
                || heads != 1 || submits != 2 || inspections != 0
                || !session.has_ambiguous_attempt())
                return false;
        }

        const auto recovered = session.retry_ambiguous();
        if (recovered.code != GaudereChatTurnCode::succeeded
            || recovered.request_id != original_request
            || recovered.task_id != new_task
            || recovered.response != "Bonjour Bertrand."
            || session.has_ambiguous_attempt()
            || session.last_verified_committed_task_id() != new_task
            || session.last_verified_response() != "Bonjour Bertrand."
            || heads != 1 || submits != (first_code == 2 ? 3 : 2)
            || inspections != 1)
            return false;
    }
    return true;
}

bool thrown_read_only_transport_preserves_session_test()
{
    // Read-only exceptions must not terminate chat or erase a proven commit.
    // Exercise both std::exception and unknown throws at every observation.
    for (const int variant : {0, 1}) {
        int heads = 0;
        int submits = 0;
        int inspections = 0;
        std::string request_id;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds budget)
            -> GaudereChatTransportReply {
            if (budget <= std::chrono::milliseconds{0})
                return {9, "unbounded read"};
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
                ++heads;
                if (heads == 1) {
                    if (variant == 0) throw std::runtime_error("head unavailable");
                    throw 42;
                }
                return {0, head_report(2, old_head)};
            }
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submits;
                request_id = command.id;
                if (!check_command_common(command, request_id))
                    return {9, "unexpected submit"};
                return {0, submit_report(3)};
            }
            if (command.operation == LiveControlOperation::inspect_task
                && command.id == new_task) {
                ++inspections;
                if (inspections <= 2) {
                    if ((inspections + variant) % 2 == 0)
                        throw std::runtime_error("read-only transport unavailable");
                    throw 42;
                }
                return {0, succeeded_task_report(request_id)};
            }
            return {9, "unexpected operation"};
        };

        GaudereChatSession session(transport, "0123456789abcdef");
        const auto before_submit = session.send_turn("Salut");
        if (before_submit.code != GaudereChatTurnCode::invalid_reply
            || before_submit.detail.find("head inspection transport threw")
                == std::string::npos
            || !before_submit.task_id.empty() || !before_submit.request_id.empty()
            || submits != 0 || session.last_verified_committed_task_id()
            || session.has_ambiguous_attempt()) return false;

        const auto committed = session.send_turn("Salut");
        if (committed.code != GaudereChatTurnCode::invalid_reply
            || committed.detail.find("Task inspection transport threw")
                == std::string::npos
            || committed.task_id != new_task || committed.request_id != request_id
            || !committed.current_revision || *committed.current_revision != 3
            || session.last_verified_committed_task_id() != new_task
            || session.last_verified_response()
            || session.has_ambiguous_attempt()
            || heads != 2 || submits != 1 || inspections != 1) return false;

        // /task ID is status-only and must not manufacture /again proof.
        const auto status = session.inspect_task(new_task);
        if (status.ok || status.task_id != new_task
            || status.detail.find("transport threw") == std::string::npos
            || session.last_verified_response()
            || heads != 2 || submits != 1 || inspections != 2) return false;

        // /wait uses the verified original identity, never another submit.
        const auto resumed = session.wait_last_committed();
        if (resumed.code != GaudereChatTurnCode::succeeded
            || resumed.task_id != new_task || resumed.request_id != request_id
            || resumed.response != "Bonjour Bertrand."
            || session.last_verified_response() != "Bonjour Bertrand."
            || heads != 2 || submits != 1 || inspections != 3
            || session.has_ambiguous_attempt()) return false;
    }
    return true;
}

bool task_inspection_io_timeout_test()
{
    int step = 0;
    int submits = 0;
    std::string request_id;
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds budget) {
        ++step;
        if (budget != std::chrono::milliseconds{3})
            return GaudereChatTransportReply{9, "per-call I/O bound was exceeded"};
        if (step == 1
            && command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (step == 2
            && command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (step == 3 && command.operation == LiveControlOperation::inspect_task
            && command.id == new_task) {
            return GaudereChatTransportReply{
                1, "gaudere-control: live control receive timed out\n"};
        }
        if (step == 4 && command.operation == LiveControlOperation::inspect_task
            && command.id == new_task) {
            return GaudereChatTransportReply{
                0, succeeded_task_report(request_id)};
        }
        return GaudereChatTransportReply{9, "unexpected command"};
    };

    gaudere_agent::GaudereChatOptions options;
    options.io_timeout = std::chrono::milliseconds{3};
    options.timeout = std::chrono::milliseconds{100};
    GaudereChatSession session(
        transport, "0011aabbccddeeff", options);
    const auto first = session.send_turn("Salut");
    if (step != 3 || submits != 1
        || first.code != GaudereChatTurnCode::timeout
        || first.task_id != new_task || first.request_id != request_id
        || !session.last_verified_committed_task_id()
        || *session.last_verified_committed_task_id() != new_task
        || session.last_verified_response()
        || !first.response.empty() || session.has_ambiguous_attempt()
        || first.detail.find("receive timed out") == std::string::npos)
        return false;

    // A per-socket timeout is not evidence of missing durable commit.
    // An explicit /wait recovers the same Task without another submit.
    const auto resumed = session.wait_last_committed();
    return step == 4 && submits == 1
        && resumed.code == GaudereChatTurnCode::succeeded
        && resumed.task_id == new_task
        && resumed.request_id == request_id
        && resumed.response == "Bonjour Bertrand."
        && session.last_verified_response() == "Bonjour Bertrand."
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
            // Each socket I/O is limited to 4 ms, while the aggregate
            // monotonic wait still expires after exactly 12 ms.
            std::chrono::milliseconds{4}, std::chrono::milliseconds{4},
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

    if (session.last_verified_response()) return false;
    const auto resumed = session.wait_last_committed();
    return resumed.code == GaudereChatTurnCode::succeeded
        && session.last_verified_response()
        && *session.last_verified_response() == "Bonjour Bertrand."
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
    return calls == 0 && !session.last_verified_response()
        && !session.last_verified_committed_task_id()
        && result.code == GaudereChatTurnCode::invalid_reply
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
        && !session.last_verified_committed_task_id()
        && !session.last_verified_response()
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
        && !session.last_verified_response()
        && resumed.detail.find("not canonical v3") != std::string::npos
        && !session.has_ambiguous_attempt();
}

bool status_only_lookup_cannot_create_last_verified_commit_test()
{
    int calls = 0;
    auto transport = [&](const LiveControlCommand& command,
                         const std::chrono::milliseconds) {
        ++calls;
        if (command.operation != LiveControlOperation::inspect_task)
            return GaudereChatTransportReply{9, "unexpected mutation"};
        return GaudereChatTransportReply{0, pending_task_report()};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    if (session.last_verified_committed_task_id() || calls != 0) return false;
    const auto status = session.inspect_task(new_task);
    if (!status.ok || status.status != "running" || calls != 1)
        return false;
    // Task status is not proof that THIS session committed it.
    return !session.last_verified_committed_task_id()
        && !session.last_verified_response() && calls == 1;
}

bool canonical_cache_cleared_by_new_verified_commit_test()
{
    const std::string second_task =
        "cognition.local-goose-dialogue.v3:" + std::string(64, 'a');
    auto now = std::chrono::steady_clock::time_point{};
    int heads = 0;
    int submits = 0;
    int inspections = 0;
    std::string first_request;
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds) {
        if (command.operation
                == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            ++heads;
            return GaudereChatTransportReply{
                0, heads == 1 ? head_report(2, old_head)
                              : head_report(3, new_task)};
        }
        if (command.operation
                == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submits;
            if (submits == 1) {
                first_request = command.id;
                return GaudereChatTransportReply{0, submit_report(3)};
            }
            std::string report = submit_report(4);
            const auto pos = report.find(new_task);
            if (pos == std::string::npos)
                return GaudereChatTransportReply{9, "bad fixture"};
            // Replace both commit id and preferred head ID; do not
            // manufacture a success response for the second Task.
            for (std::size_t at = report.find(new_task);
                 at != std::string::npos;
                 at = report.find(new_task, at + second_task.size())) {
                report.replace(at, new_task.size(), second_task);
            }
            return GaudereChatTransportReply{0, report};
        }
        if (command.operation == LiveControlOperation::inspect_task) {
            ++inspections;
            if (command.id == new_task)
                return GaudereChatTransportReply{
                    0, succeeded_task_report(first_request)};
            std::string pending = pending_task_report();
            const auto pos = pending.find(new_task);
            if (pos == std::string::npos)
                return GaudereChatTransportReply{9, "bad fixture"};
            pending.replace(pos, new_task.size(), second_task);
            return GaudereChatTransportReply{0, pending};
        }
        return GaudereChatTransportReply{9, "unexpected operation"};
    };
    gaudere_agent::GaudereChatOptions options;
    options.timeout = std::chrono::milliseconds{2};
    options.poll_interval = std::chrono::milliseconds{2};
    const auto clock = [&] { return now; };
    const auto sleeper = [&](std::chrono::milliseconds delay) { now += delay; };
    GaudereChatSession session(transport, "0123456789abcdef", options, clock, sleeper);

    const auto first = session.send_turn("Salut");
    if (first.code != GaudereChatTurnCode::succeeded
        || session.last_verified_response() != "Bonjour Bertrand."
        || session.last_verified_committed_task_id() != new_task)
        return false;
    const auto second = session.send_turn("Autre");
    return second.code == GaudereChatTurnCode::timeout
        && second.task_id == second_task
        && session.last_verified_committed_task_id() == second_task
        && !session.last_verified_response()
        && heads == 2 && submits == 2 && inspections == 2;
}

bool slash_command_routing_test()
{
    using gaudere_agent::GaudereChatInputKind;
    using gaudere_agent::parse_gaudere_chat_input;
    struct Case {
        std::string line;
        GaudereChatInputKind kind;
        std::string payload;
    };
    const std::vector<Case> cases{
        {"", GaudereChatInputKind::empty, ""},
        {"Bonjour", GaudereChatInputKind::text, "Bonjour"},
        {"  /wai", GaudereChatInputKind::text, "  /wai"},
        {"/help", GaudereChatInputKind::help, ""},
        {"/head", GaudereChatInputKind::head, ""},
        {"/last", GaudereChatInputKind::last, ""},
        {"/again", GaudereChatInputKind::again, ""},
        {"/wait", GaudereChatInputKind::wait, ""},
        {"/retry", GaudereChatInputKind::retry, ""},
        {"/quit", GaudereChatInputKind::quit, ""},
        {"/task " + new_task, GaudereChatInputKind::task, new_task},
        {"//help", GaudereChatInputKind::text, "/help"},
        {"//wait", GaudereChatInputKind::text, "/wait"},
        {"//last", GaudereChatInputKind::text, "/last"},
        {"//again", GaudereChatInputKind::text, "/again"},
        {"//task " + new_task, GaudereChatInputKind::text, "/task " + new_task},
        {"//", GaudereChatInputKind::text, "/"},
        {"///bonjour", GaudereChatInputKind::text, "//bonjour"},
        {"/", GaudereChatInputKind::unknown_command, "/"},
        {"/wai", GaudereChatInputKind::unknown_command, "/wai"},
        {"/HEAD", GaudereChatInputKind::unknown_command, "/HEAD"},
        {"/help now", GaudereChatInputKind::unknown_command, "/help now"},
        {"/head now", GaudereChatInputKind::unknown_command, "/head now"},
        {"/last now", GaudereChatInputKind::unknown_command, "/last now"},
        {"/again now", GaudereChatInputKind::unknown_command, "/again now"},
        {"/AGAIN", GaudereChatInputKind::unknown_command, "/AGAIN"},
        {"/LAST", GaudereChatInputKind::unknown_command, "/LAST"},
        {"/wait now", GaudereChatInputKind::unknown_command, "/wait now"},
        {"/retry now", GaudereChatInputKind::unknown_command, "/retry now"},
        {"/quit now", GaudereChatInputKind::unknown_command, "/quit now"},
        {"/task", GaudereChatInputKind::unknown_command, "/task"},
        {"/task ", GaudereChatInputKind::unknown_command, "/task "},
        {"/task  " + new_task, GaudereChatInputKind::unknown_command, "/task  " + new_task},
        {"/task " + new_task + " extra", GaudereChatInputKind::unknown_command,
            "/task " + new_task + " extra"},
        {"/task\t" + new_task, GaudereChatInputKind::unknown_command, "/task\t" + new_task},
        {"/taskx " + new_task, GaudereChatInputKind::unknown_command,
            "/taskx " + new_task},
        {"/provider", GaudereChatInputKind::unknown_command, "/provider"},
        {"/ack", GaudereChatInputKind::unknown_command, "/ack"},
        {"/send Hello", GaudereChatInputKind::unknown_command, "/send Hello"}
    };
    for (const auto& sample : cases) {
        const auto parsed = parse_gaudere_chat_input(sample.line);
        if (parsed.kind != sample.kind || parsed.value != sample.payload)
            return false;
        // No unescaped slash-prefixed input may become a dialogue submit.
        if (sample.line.rfind("/", 0) == 0
            && sample.line.rfind("//", 0) != 0
            && parsed.kind == GaudereChatInputKind::text)
            return false;
    }
    return true;
}

bool malformed_post_commit_observation_preserves_recovery_test()
{
    // Every malformed or non-canonical Task report is an observation failure,
    // not evidence that the previously verified durable submit did not happen.
    const std::vector<std::string> bad_reports{
        "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"
            "status=\"bad\\q\"\n",  // malformed escaped field
        "id=\"" + old_head + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"
            "status=running\n",   // wrong Task ID
        "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"
            "status=succeeded\n", // absent result envelope
        succeeded_task_report("wrong-request-id"),  // non-canonical reply
        "id=\"" + new_task + "\"\n"
            "kind=\"cognition.local-goose-dialogue.v3\"\n"
            "status=unexpected\n" // unknown Task status
    };
    for (const auto& bad_report : bad_reports) {
        int submissions = 0;
        int inspections = 0;
        std::string request_id;
        auto transport = [&](const LiveControlCommand& command,
                             const std::chrono::milliseconds) {
            if (command.operation
                == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            }
            if (command.operation
                == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submissions;
                request_id = command.id;
                return GaudereChatTransportReply{0, submit_report(3)};
            }
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task) {
                return GaudereChatTransportReply{9, "unexpected operation"};
            }
            ++inspections;
            if (inspections == 1)
                return GaudereChatTransportReply{0, bad_report};
            return GaudereChatTransportReply{
                0, succeeded_task_report(request_id)};
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto bad = session.send_turn("Salut");
        if (bad.code != GaudereChatTurnCode::invalid_reply
            || bad.task_id != new_task || bad.request_id != request_id
            || bad.expected_revision != 2
            || !bad.current_revision || *bad.current_revision != 3
            || !bad.response.empty()
            || bad.detail.empty() || submissions != 1
            || inspections != 1 || session.has_ambiguous_attempt())
            return false;

        // A deliberate read-only re-observation of exactly the same commit
        // can retrieve a later canonical result; it must NEVER re-submit.
        const auto recovered = session.wait_last_committed();
        if (recovered.code != GaudereChatTurnCode::succeeded
            || recovered.task_id != new_task
            || recovered.request_id != request_id
            || recovered.response != "Bonjour Bertrand."
            || submissions != 1 || inspections != 2
            || session.has_ambiguous_attempt())
            return false;
    }
    return true;
}

bool malformed_unverified_commit_has_no_recovery_identity_test()
{
    int heads = 0;
    int submissions = 0;
    int inspections = 0;
    std::string first_request;
    auto transport = [&](const LiveControlCommand& command,
                         const std::chrono::milliseconds) {
        if (command.operation
            == LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            ++heads;
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (command.operation
            == LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submissions;
            if (submissions == 1) {
                first_request = command.id;
                return GaudereChatTransportReply{0, "status=pending\n"};
            }
            if (command.id != first_request
                || !check_command_common(command, first_request)) {
                return GaudereChatTransportReply{9, "retry changed request identity"};
            }
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        ++inspections;
        if (command.operation != LiveControlOperation::inspect_task
            || command.id != new_task) {
            return GaudereChatTransportReply{9, "unexpected Task inspection"};
        }
        return GaudereChatTransportReply{
            0, succeeded_task_report(first_request)};
    };
    GaudereChatSession session(transport, "0123456789abcdef");
    const auto bad = session.send_turn("Salut");
    if (bad.code != GaudereChatTurnCode::transport_ambiguous
        || !bad.task_id.empty() || bad.current_revision
        || !bad.response.empty() || submissions != 1 || heads != 1
        || inspections != 0 || bad.request_id != first_request
        || !session.has_ambiguous_attempt()
        || session.ambiguous_message() != "Salut"
        || session.last_verified_committed_task_id()
        || session.last_verified_response()) return false;

    // A successful transport response with an unverifiable commit is NOT
    // proof that it was rejected. Blocking a different message must be local.
    const auto blocked = session.send_turn("Autre");
    const auto wait = session.wait_last_committed();
    if (blocked.code != GaudereChatTurnCode::transport_ambiguous
        || blocked.request_id != first_request
        || wait.code != GaudereChatTurnCode::invalid_reply
        || !wait.task_id.empty()
        || heads != 1 || submissions != 1 || inspections != 0)
        return false;

    // Only the user's explicit retry can resend, and it must preserve all
    // original submission fields and idempotency key exactly.
    const auto retried = session.retry_ambiguous();
    return retried.code == GaudereChatTurnCode::succeeded
        && retried.request_id == first_request
        && retried.task_id == new_task
        && retried.response == "Bonjour Bertrand."
        && heads == 1 && submissions == 2 && inspections == 1
        && !session.has_ambiguous_attempt()
        && session.last_verified_committed_task_id() == new_task
        && session.last_verified_response() == "Bonjour Bertrand.";
}

bool duplicate_head_key_rejected_before_submit_test()
{
    for (const auto& extra : {
             "alias=\"main\"\n", "revision=2\n",
             "root_task_id=\"cognition.local-goose-dialogue.v2:bad\"\n",
             "head_task_id=\"cognition.local-goose-dialogue.v3:bad\"\n",
             "unused=one\nunused=two\n"}) {
        int reads = 0;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            ++reads;
            if (command.operation !=
                LiveControlOperation::inspect_local_goose_dialogue_thread_head)
                return GaudereChatTransportReply{9, "unwanted submit"};
            return GaudereChatTransportReply{0, head_report(2, old_head) + extra};
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto result = session.send_turn("Salut");
        if (reads != 1 || result.code != GaudereChatTurnCode::invalid_reply
            || !result.task_id.empty() || result.current_revision
            || session.has_ambiguous_attempt()) return false;
    }
    return true;
}

bool duplicate_commit_key_rejected_without_fake_commit_test()
{
    for (const auto& extra : {
             "id=\"cognition.local-goose-dialogue.v3:bad\"\n",
             "kind=\"cognition.local-goose-dialogue.v3\"\n",
             "revision=3\n", "alias=\"main\"\n",
             "status=pending\n", "attempts=0/2\n"}) {
        int submits = 0;
        int inspections = 0;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head)
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submits;
                return GaudereChatTransportReply{0, submit_report(3) + extra};
            }
            ++inspections;
            return GaudereChatTransportReply{9, "unwanted inspection"};
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto result = session.send_turn("Salut");
        const auto wait = session.wait_last_committed();
        const auto blocked = session.send_turn("Autre");
        if (submits != 1 || inspections != 0
            || result.code != GaudereChatTurnCode::transport_ambiguous
            || !result.task_id.empty() || result.current_revision
            || !result.response.empty()
            || !session.has_ambiguous_attempt()
            || blocked.code != GaudereChatTurnCode::transport_ambiguous
            || blocked.request_id != result.request_id
            || wait.code != GaudereChatTurnCode::invalid_reply
            || !wait.task_id.empty()
            || session.last_verified_committed_task_id()
            || session.last_verified_response()) return false;
    }
    return true;
}

bool mismatched_positive_commit_report_remains_ambiguous_test()
{
    // A syntactically sound but semantically wrong success report is still
    // possibly post-side-effect. Never upgrade it to a verified commit.
    const std::string false_v2_task =
        "cognition.local-goose-dialogue.v2:" + std::string(64, 'd');
    const std::vector<std::string> wrong_reports{
        submit_report(4), // wrong committed revision
        // An ID with valid V2 syntax cannot represent a new V3 commit.
        "id=\"" + false_v2_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "status=pending\n"
        "alias=\"main\"\n"
        "revision=3\n"
        "root_task_id=\"" + root + "\"\n"
        "head_task_id=\"" + false_v2_task + "\"\n",
        // Truncated or unrecognized status cannot grant commit proof.
        "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "alias=\"main\"\n"
        "revision=3\n"
        "root_task_id=\"" + root + "\"\n"
        "head_task_id=\"" + new_task + "\"\n",
        "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "status=unverified\n"
        "alias=\"main\"\n"
        "revision=3\n"
        "root_task_id=\"" + root + "\"\n"
        "head_task_id=\"" + new_task + "\"\n",
        "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "alias=\"other\"\n"
        "revision=3\n"
        "root_task_id=\"" + old_head + "\"\n"
        "head_task_id=\"" + new_task + "\"\n",
        "id=\"" + new_task + "\"\n"
        "kind=\"cognition.local-goose-dialogue.v3\"\n"
        "alias=\"main\"\n"
        "revision=3\n"
        "root_task_id=\"" + old_head + "\"\n"
        "head_task_id=\"" + old_head + "\"\n"
    };
    for (const auto& wrong : wrong_reports) {
        int calls = 0;
        std::string request_id;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            ++calls;
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head)
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                request_id = command.id;
                return GaudereChatTransportReply{0, wrong};
            }
            return GaudereChatTransportReply{9, "unexpected Task inspection"};
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto result = session.send_turn("Salut");
        const auto blocked = session.send_turn("Autre");
        if (calls != 2 || result.code != GaudereChatTurnCode::transport_ambiguous
            || result.request_id != request_id || result.task_id != ""
            || !session.has_ambiguous_attempt()
            || blocked.code != GaudereChatTurnCode::transport_ambiguous
            || blocked.request_id != request_id
            || session.last_verified_committed_task_id()
            || session.last_verified_response())
            return false;
    }
    return true;
}

bool duplicate_task_key_rejected_with_proven_identity_test()
{
    for (int variant = 0; variant != 5; ++variant) {
        int submits = 0;
        int reads = 0;
        std::string request_id;
        const auto transport = [&](const LiveControlCommand& command,
                                   const std::chrono::milliseconds) {
            if (command.operation ==
                LiveControlOperation::inspect_local_goose_dialogue_thread_head)
                return GaudereChatTransportReply{0, head_report(2, old_head)};
            if (command.operation ==
                LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
                ++submits;
                request_id = command.id;
                return GaudereChatTransportReply{0, submit_report(3)};
            }
            if (command.operation != LiveControlOperation::inspect_task
                || command.id != new_task)
                return GaudereChatTransportReply{9, "unwanted operation"};
            if (++reads == 2)
                return GaudereChatTransportReply{0, succeeded_task_report(request_id)};
            const std::string extra = variant == 0
                ? "id=\"" + old_head + "\"\n"
                : variant == 1 ? "status=succeeded\n"
                : variant == 2 ? "kind=\"cognition.local-goose-dialogue.v3\"\n"
                : variant == 3 ? "result_output=" +
                    quote_report(response_json(request_id)) + "\n"
                : "attempts=1/2\n";
            const std::string base = variant == 3
                ? succeeded_task_report(request_id) : pending_task_report();
            return GaudereChatTransportReply{0, base + extra};
        };
        GaudereChatSession session(transport, "0123456789abcdef");
        const auto invalid = session.send_turn("Salut");
        if (invalid.code != GaudereChatTurnCode::invalid_reply
            || invalid.task_id != new_task || !invalid.current_revision
            || *invalid.current_revision != 3 || !invalid.response.empty()
            || submits != 1 || reads != 1) return false;
        const auto recovered = session.wait_last_committed();
        if (recovered.code != GaudereChatTurnCode::succeeded
            || recovered.task_id != new_task
            || recovered.response != "Bonjour Bertrand."
            || submits != 1 || reads != 2) return false;
    }
    return true;
}

bool zero_turn_successor_response_never_populates_cache_test()
{
    int heads = 0;
    int submissions = 0;
    int inspections = 0;
    std::string request_id;
    const auto transport = [&](const LiveControlCommand& command,
                               const std::chrono::milliseconds) {
        if (command.operation ==
            LiveControlOperation::inspect_local_goose_dialogue_thread_head) {
            ++heads;
            return GaudereChatTransportReply{0, head_report(2, old_head)};
        }
        if (command.operation ==
            LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next) {
            ++submissions;
            request_id = command.id;
            return GaudereChatTransportReply{0, submit_report(3)};
        }
        if (command.operation != LiveControlOperation::inspect_task
            || command.id != new_task)
            return GaudereChatTransportReply{9, "unexpected operation"};
        ++inspections;
        if (inspections == 1) {
            auto forged = Json::parse(response_json(request_id));
            forged["turn_index"] = 0;
            // JSON is canonical and all request/lineage IDs are unchanged.
            // Only the root-only index violates a preferred-next successor.
            return GaudereChatTransportReply{0,
                "id=\"" + new_task + "\"\n"
                "kind=\"cognition.local-goose-dialogue.v3\"\n"
                "status=succeeded\n"
                "result_content_type=\"application/vnd.gaudere.local-goose-dialogue-v3-response+json\"\n"
                "result_output=" + quote_report(forged.dump()) + "\n"};
        }
        return GaudereChatTransportReply{
            0, succeeded_task_report(request_id)};
    };

    GaudereChatSession session(transport, "0123456789abcdef");
    const auto first = session.send_turn("Salut");
    if (first.code != GaudereChatTurnCode::invalid_reply
        || first.task_id != new_task || !first.response.empty()
        || first.detail.find("not canonical v3") == std::string::npos
        || heads != 1 || submissions != 1 || inspections != 1
        || session.has_ambiguous_attempt()
        || session.last_verified_committed_task_id() != new_task
        || session.last_verified_response())
        return false;

    // /wait is read-only, uses the same proven commit, and can recover a
    // later genuinely canonical reply without a second provider submit.
    const auto recovered = session.wait_last_committed();
    return recovered.code == GaudereChatTurnCode::succeeded
        && recovered.task_id == new_task
        && recovered.response == "Bonjour Bertrand."
        && session.last_verified_response() == "Bonjour Bertrand."
        && heads == 1 && submissions == 1 && inspections == 2;
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
        {"v2_task_id_in_conflict_is_not_v3_orphan_test",
         v2_task_id_in_conflict_is_not_v3_orphan_test},
        {"submit_protocol_error_retains_exact_retry_test",
         submit_protocol_error_retains_exact_retry_test},
        {"thrown_submit_transport_preserves_exact_retry_test",
         thrown_submit_transport_preserves_exact_retry_test},
        {"thrown_read_only_transport_preserves_session_test",
         thrown_read_only_transport_preserves_session_test},
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
        {"slash_command_routing_test", slash_command_routing_test},
        {"canonical_cache_cleared_by_new_verified_commit_test",
         canonical_cache_cleared_by_new_verified_commit_test},
        {"status_only_lookup_cannot_create_last_verified_commit_test",
         status_only_lookup_cannot_create_last_verified_commit_test},
        {"malformed_post_commit_observation_preserves_recovery_test",
         malformed_post_commit_observation_preserves_recovery_test},
        {"malformed_unverified_commit_has_no_recovery_identity_test",
         malformed_unverified_commit_has_no_recovery_identity_test},
        {"mismatched_positive_commit_report_remains_ambiguous_test",
         mismatched_positive_commit_report_remains_ambiguous_test},
        {"duplicate_head_key_rejected_before_submit_test",
         duplicate_head_key_rejected_before_submit_test},
        {"duplicate_commit_key_rejected_without_fake_commit_test",
         duplicate_commit_key_rejected_without_fake_commit_test},
        {"duplicate_task_key_rejected_with_proven_identity_test",
         duplicate_task_key_rejected_with_proven_identity_test},
        {"zero_turn_successor_response_never_populates_cache_test",
         zero_turn_successor_response_never_populates_cache_test},
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
