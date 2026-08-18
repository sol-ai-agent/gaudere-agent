#include "LocalWaitHandler.hpp"

#include <gaudere/work/Task.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main()
{
    using namespace std::chrono_literals;
    using namespace gaudere_agent;

    expect(parse_local_wait_duration("1") == 1ms,
           "minimum wait duration parses");
    expect(parse_local_wait_duration("5000") == 5000ms,
           "maximum wait duration parses");
    expect(!parse_local_wait_duration("0"),
           "zero wait duration is rejected");
    expect(!parse_local_wait_duration("5001"),
           "oversized wait duration is rejected");
    expect(!parse_local_wait_duration("10ms"),
           "non-integer wait duration is rejected");

    gaudere::work::Task task;
    task.input = "20";
    LocalWaitHandler handler;
    const TaskContext success_context{task, [] { return false; }};
    const auto started = std::chrono::steady_clock::now();
    const auto success = handler.execute(success_context);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(success.outcome == HandlerOutcome::succeeded
               && success.output == "waited 20 ms",
           "local wait completes deterministically");
    expect(elapsed >= 15ms,
           "local wait actually waits approximately the requested duration");

    const TaskContext cancelled_context{task, [] { return true; }};
    const auto cancelled = handler.execute(cancelled_context);
    expect(cancelled.outcome == HandlerOutcome::cancelled,
           "local wait cooperatively acknowledges cancellation");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All local wait handler tests passed\n";
    return 0;
}
