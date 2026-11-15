#include "GaudereChat.hpp"
#include "GaudereChatInput.hpp"

#include "LiveControl.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

std::int64_t parse_positive_ms(const std::string& value, const char* name)
{
    std::size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed, 10);
    if (consumed != value.size() || parsed <= 0 || parsed > 3600000) {
        throw std::invalid_argument(std::string{name} + " must be 1..3600000 ms");
    }
    return parsed;
}

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " [--socket PATH] [--thread ALIAS] [--poll-ms N]"
        << " [--timeout-ms N] [--io-timeout-ms N] [--verbose]\n";
}

void help()
{
    std::cout
        << "/help   show local commands\n"
        << "/head   inspect the current preferred thread head\n"
        << "/last   show the last locally verified committed Task ID (no I/O)\n"
        << "/task ID  inspect one previously submitted V3 Task (read-only)\n"
        << "/wait   wait again for the last verified committed Task (read-only)\n"
        << "/retry  explicitly retry the last conflicting or ambiguous message\n"
        << "/quit   leave gaudere-chat without submitting anything\n"
        << "//text  send text starting with a literal / (escape commands)\n";
}

void print_head(const gaudere_agent::GaudereChatHead& head)
{
    std::cout
        << "thread=" << head.alias
        << " revision=" << head.revision
        << " head=" << head.head_task_id << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        std::string socket_path = "/tmp/gaudere-control.sock";
        gaudere_agent::GaudereChatOptions options;
        bool verbose = false;

        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--socket" && index + 1 < argc) {
                socket_path = argv[++index];
            } else if (argument == "--thread" && index + 1 < argc) {
                options.thread_alias = argv[++index];
            } else if (argument == "--poll-ms" && index + 1 < argc) {
                options.poll_interval = std::chrono::milliseconds{
                    parse_positive_ms(argv[++index], "--poll-ms")};
            } else if (argument == "--timeout-ms" && index + 1 < argc) {
                options.timeout = std::chrono::milliseconds{
                    parse_positive_ms(argv[++index], "--timeout-ms")};
            } else if (argument == "--io-timeout-ms" && index + 1 < argc) {
                options.io_timeout = std::chrono::milliseconds{
                    parse_positive_ms(argv[++index], "--io-timeout-ms")};
            } else if (argument == "--verbose") {
                verbose = true;
            } else if (argument == "--help") {
                usage(argv[0]);
                help();
                return 0;
            } else {
                usage(argv[0]);
                return 2;
            }
        }

        auto transport = [&socket_path](
            const gaudere_agent::LiveControlCommand& command,
            const std::chrono::milliseconds remaining_budget) {
            std::ostringstream output;
            std::ostringstream error;
            const int code = gaudere_agent::run_live_control_client(
                socket_path, command, output, error, remaining_budget);
            std::string body = code == 0 ? output.str() : error.str();
            if (body.empty()) body = output.str() + error.str();
            return gaudere_agent::GaudereChatTransportReply{
                code, std::move(body)};
        };

        gaudere_agent::GaudereChatSession session(
            std::move(transport),
            gaudere_agent::random_gaudere_chat_session_nonce(),
            options);

        std::string conflict_message;
        std::cout << "gaudere-chat thread=" << options.thread_alias
                  << " (type /help for local commands)\n";

        for (;;) {
            std::cout << "Bertrand> " << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) {
                std::cout << '\n';
                break;
            }
            const auto input = gaudere_agent::parse_gaudere_chat_input(line);
            using InputKind = gaudere_agent::GaudereChatInputKind;
            if (input.kind == InputKind::empty) continue;
            if (input.kind == InputKind::quit) break;
            if (input.kind == InputKind::help) {
                help();
                continue;
            }
            if (input.kind == InputKind::head) {
                const auto head = session.inspect_head();
                if (head.ok) print_head(head.head);
                else std::cerr << "gaudere-chat: " << head.detail;
                continue;
            }
            if (input.kind == InputKind::last) {
                // Snapshot solely of this process's already verified commit.
                // Neither query the socket nor infer a commit from status.
                const auto task_id = session.last_verified_committed_task_id();
                if (task_id) {
                    std::cout << "last verified committed task=" << *task_id
                              << " (in-memory only; /wait to resume, /task "
                              << *task_id << " for status only)\n";
                } else {
                    std::cout << "gaudere-chat: no verified committed Task in this session\n";
                }
                continue;
            }
            if (input.kind == InputKind::task) {
                const auto report = session.inspect_task(input.value);
                if (report.ok) {
                    std::cout << "task=" << report.task_id
                              << " status=" << report.status << '\n';
                } else {
                    std::cerr << "gaudere-chat: read-only Task inspection failed: "
                              << report.detail << '\n';
                }
                continue;
            }
            if (input.kind == InputKind::unknown_command) {
                std::cerr << "gaudere-chat: unknown local command; "
                             "use /help (or // to send a literal slash)\n";
                continue;
            }

            gaudere_agent::GaudereChatTurnResult result;
            std::string attempted_message;
            if (input.kind == InputKind::wait) {
                // Read-only bounded re-observation; no submit and no ACK.
                result = session.wait_last_committed();
            } else if (input.kind == InputKind::retry) {
                if (session.has_ambiguous_attempt()) {
                    attempted_message = session.ambiguous_message();
                    result = session.retry_ambiguous();
                } else if (!conflict_message.empty()) {
                    attempted_message = conflict_message;
                    conflict_message.clear();
                    result = session.send_turn(attempted_message);
                } else {
                    std::cerr << "gaudere-chat: nothing requires an explicit retry\n";
                    continue;
                }
            } else {
                if (session.has_ambiguous_attempt()) {
                    std::cerr
                        << "gaudere-chat: the previous submission has ambiguous "
                        << "transport state; use /retry before sending new text\n";
                    continue;
                }
                conflict_message.clear();
                attempted_message = input.value;
                result = session.send_turn(attempted_message);
            }

            if (verbose) {
                std::cerr
                    << "gaudere-chat: request=" << result.request_id;
                if (!result.task_id.empty()) {
                    std::cerr << " task=" << result.task_id;
                }
                std::cerr << " expected_revision=" << result.expected_revision;
                if (result.current_revision) {
                    std::cerr << " current_revision=" << *result.current_revision;
                }
                std::cerr << '\n';
            }

            switch (result.code) {
            case gaudere_agent::GaudereChatTurnCode::succeeded:
                conflict_message.clear();
                std::cout << "Gaudere> " << result.response << '\n';
                break;
            case gaudere_agent::GaudereChatTurnCode::conflict:
                conflict_message = attempted_message;
                std::cerr
                    << "gaudere-chat: preferred thread advanced concurrently; "
                    << "the message was not committed to the preferred thread";
                if (result.orphan_task_may_exist) {
                    std::cerr << " (a durable branch Task may exist: "
                              << result.task_id << ')';
                }
                std::cerr << ". Use /retry to resubmit explicitly against the "
                             "fresh head.\n";
                break;
            case gaudere_agent::GaudereChatTurnCode::transport_ambiguous:
                std::cerr
                    << "gaudere-chat: submission/inspection transport state is "
                    << "ambiguous. Do not create a new message; use /retry for "
                    << "the exact preserved attempt.\n"
                    << result.detail;
                break;
            case gaudere_agent::GaudereChatTurnCode::timeout:
                std::cerr
                    << "gaudere-chat: Task " << result.task_id
                    << " is durably submitted but could not be observed as "
                    << "terminal within the bounded wait. Do not resubmit this "
                    << "message. Use /wait to await the last verified "
                    << "commit again in this session, or /task "
                    << result.task_id << " to inspect its status read-only.\n";
                break;
            case gaudere_agent::GaudereChatTurnCode::terminal_failure:
                std::cerr
                    << "gaudere-chat: Task " << result.task_id
                    << " ended without a canonical success.\n"
                    << result.detail;
                break;
            case gaudere_agent::GaudereChatTurnCode::invalid_reply:
                std::cerr << "gaudere-chat: " << result.detail << '\n';
                if (!result.task_id.empty()) {
                    // The commit was confirmed before observation failed.
                    // Do NOT interpret invalid response syntax as no submit.
                    std::cerr << "gaudere-chat: Task " << result.task_id
                              << " was durably submitted; do not resubmit. "
                              << "Use /wait in this session or /task "
                              << result.task_id
                              << " to inspect its status read-only.\n";
                }
                break;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-chat: " << error.what() << '\n';
        return 1;
    }
}
