#include "GaudereChat.hpp"

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
        << "/retry  explicitly retry the last conflicting or ambiguous message\n"
        << "/quit   leave gaudere-chat without submitting anything\n";
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

        auto transport = [&socket_path, &options](
            const gaudere_agent::LiveControlCommand& command) {
            std::ostringstream output;
            std::ostringstream error;
            const int code = gaudere_agent::run_live_control_client(
                socket_path, command, output, error, options.io_timeout);
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
            if (line.empty()) continue;
            if (line == "/quit") break;
            if (line == "/help") {
                help();
                continue;
            }
            if (line == "/head") {
                const auto head = session.inspect_head();
                if (head.ok) print_head(head.head);
                else std::cerr << "gaudere-chat: " << head.detail;
                continue;
            }

            gaudere_agent::GaudereChatTurnResult result;
            std::string attempted_message;
            if (line == "/retry") {
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
                attempted_message = line;
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
                    << " is durably submitted but still non-terminal after the "
                    << "bounded wait. Do not resubmit this message.\n";
                break;
            case gaudere_agent::GaudereChatTurnCode::terminal_failure:
                std::cerr
                    << "gaudere-chat: Task " << result.task_id
                    << " ended without a canonical success.\n"
                    << result.detail;
                break;
            case gaudere_agent::GaudereChatTurnCode::invalid_reply:
                std::cerr << "gaudere-chat: " << result.detail << '\n';
                break;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-chat: " << error.what() << '\n';
        return 1;
    }
}
