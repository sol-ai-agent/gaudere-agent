#ifndef GAUDERE_AGENT_GAUDERE_CHAT_HPP
#define GAUDERE_AGENT_GAUDERE_CHAT_HPP

#include "LiveControl.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

struct GaudereChatTransportReply {
    int code = 1;
    std::string body;
};

using GaudereChatTransport =
    std::function<GaudereChatTransportReply(const LiveControlCommand&)>;
using GaudereChatClock =
    std::function<std::chrono::steady_clock::time_point()>;
using GaudereChatSleeper =
    std::function<void(std::chrono::milliseconds)>;

struct GaudereChatOptions {
    std::string thread_alias = "main";
    std::chrono::milliseconds poll_interval{250};
    std::chrono::milliseconds timeout{600000};
};

struct GaudereChatHead {
    std::string alias;
    std::uint64_t revision = 0;
    std::string root_task_id;
    std::string head_task_id;
};

struct GaudereChatHeadResult {
    bool ok = false;
    GaudereChatHead head;
    std::string detail;
};

enum class GaudereChatTurnCode {
    succeeded,
    conflict,
    timeout,
    terminal_failure,
    transport_ambiguous,
    invalid_reply
};

struct GaudereChatTurnResult {
    GaudereChatTurnCode code = GaudereChatTurnCode::invalid_reply;
    std::string request_id;
    std::string task_id;
    std::string response;
    std::string detail;
    std::uint64_t expected_revision = 0;
    std::optional<std::uint64_t> current_revision;
    bool orphan_task_may_exist = false;
};

class GaudereChatSession {
public:
    GaudereChatSession(
        GaudereChatTransport transport,
        std::string session_nonce,
        GaudereChatOptions options = {},
        GaudereChatClock clock = {},
        GaudereChatSleeper sleeper = {});

    [[nodiscard]] GaudereChatHeadResult inspect_head() const;
    [[nodiscard]] GaudereChatTurnResult send_turn(const std::string& message);
    [[nodiscard]] GaudereChatTurnResult retry_ambiguous();

    [[nodiscard]] bool has_ambiguous_attempt() const noexcept;
    [[nodiscard]] std::string ambiguous_message() const;

private:
    struct Attempt {
        std::string request_id;
        std::string message;
        GaudereChatHead head;
    };

    [[nodiscard]] GaudereChatTurnResult submit_attempt(const Attempt& attempt);
    [[nodiscard]] GaudereChatTurnResult wait_for_task(
        const Attempt& attempt,
        const std::string& task_id);
    [[nodiscard]] std::string next_request_id(const std::string& message);

    GaudereChatTransport transport_;
    std::string session_nonce_;
    GaudereChatOptions options_;
    GaudereChatClock clock_;
    GaudereChatSleeper sleeper_;
    std::uint64_t next_turn_ = 1;
    std::optional<Attempt> ambiguous_attempt_;
};

[[nodiscard]] std::string random_gaudere_chat_session_nonce();

} // namespace gaudere_agent

#endif
