#pragma once

#include <string>
#include <utility>

namespace gaudere_agent {

// Parse one terminal input line BEFORE any connection or Task operation.
// Leading slash is reserved for exact local commands. A literal message
// starting with slash must be escaped as //message; the first slash is
// removed exactly once. This prevents misspelled commands from becoming
// durable preferred-thread V3 submissions by accident.
enum class GaudereChatInputKind {
    empty,
    text,
    help,
    head,
    last,
    again,
    task,
    wait,
    retry,
    quit,
    unknown_command
};

struct GaudereChatInput {
    GaudereChatInputKind kind = GaudereChatInputKind::empty;
    std::string value;
};

[[nodiscard]] inline GaudereChatInput parse_gaudere_chat_input(
    const std::string& line)
{
    if (line.empty()) return {};
    if (line.front() != '/') return {GaudereChatInputKind::text, line};
    if (line.size() >= 2 && line[1] == '/') {
        return {GaudereChatInputKind::text, line.substr(1)};
    }

    if (line == "/help") return {GaudereChatInputKind::help, {}};
    if (line == "/head") return {GaudereChatInputKind::head, {}};
    if (line == "/last") return {GaudereChatInputKind::last, {}};
    if (line == "/again") return {GaudereChatInputKind::again, {}};
    if (line == "/wait") return {GaudereChatInputKind::wait, {}};
    if (line == "/retry") return {GaudereChatInputKind::retry, {}};
    if (line == "/quit") return {GaudereChatInputKind::quit, {}};
    if (line.rfind("/task ", 0) == 0) {
        const auto id = line.substr(6);
        // The actual Task ID is strictly validated again by inspect_task;
        // this syntax gate rejects empty/multiple/split CLI arguments.
        if (!id.empty() && id.find_first_of(" \t\r\n") == std::string::npos) {
            return {GaudereChatInputKind::task, id};
        }
    }
    return {GaudereChatInputKind::unknown_command, line};
}

} // namespace gaudere_agent
