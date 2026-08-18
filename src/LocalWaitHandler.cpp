#include "LocalWaitHandler.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <string>
#include <thread>

namespace gaudere_agent {

std::optional<std::chrono::milliseconds>
parse_local_wait_duration(const std::string_view input) noexcept
{
    if (input.empty()) {
        return std::nullopt;
    }
    std::uint64_t milliseconds = 0;
    const auto result = std::from_chars(
        input.data(), input.data() + input.size(), milliseconds);
    if (result.ec != std::errc{} || result.ptr != input.data() + input.size()
        || milliseconds == 0 || milliseconds > 5000) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{milliseconds};
}

HandlerResult LocalWaitHandler::execute(const TaskContext& context)
{
    const auto duration = parse_local_wait_duration(context.task.input);
    if (!duration) {
        return HandlerResult{HandlerOutcome::failed, {}, {},
                             "invalid_wait_duration",
                             "local.wait requires an integer duration from 1 to 5000 ms"};
    }

    const auto deadline = std::chrono::steady_clock::now() + *duration;
    for (;;) {
        if (context.cancellation_requested()) {
            return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds{10}));
    }

    return HandlerResult{HandlerOutcome::succeeded, "text/plain",
                         "waited " + std::to_string(duration->count()) + " ms",
                         {}, {}};
}

} // namespace gaudere_agent
