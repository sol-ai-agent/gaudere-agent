#ifndef GAUDERE_AGENT_LOCAL_GOOSE_RUNNER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_RUNNER_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace gaudere_agent {

enum class LocalGooseRunOutcome { succeeded, failed, timed_out, output_too_large };

struct LocalGooseRunRequest {
    std::string model_path;
    std::string prompt;
    std::chrono::milliseconds timeout{std::chrono::minutes{10}};
    std::size_t max_output_bytes = 16 * 1024;
};

struct LocalGooseRunResult {
    LocalGooseRunOutcome outcome = LocalGooseRunOutcome::failed;
    std::string output;
    std::string detail;
};

struct GooseCliInvocation {
    std::string binary;
    std::vector<std::string> argv;
    std::vector<std::string> environment;
};

[[nodiscard]] GooseCliInvocation make_goose_cli_invocation(
    const LocalGooseRunRequest& request);

class LocalGooseRunner {
public:
    virtual ~LocalGooseRunner() = default;
    [[nodiscard]] virtual LocalGooseRunResult run(
        const LocalGooseRunRequest& request) = 0;
};

/** Fixed, no-shell Goose invocation. The production container remains Network=none. */
class PosixLocalGooseRunner final : public LocalGooseRunner {
public:
    [[nodiscard]] LocalGooseRunResult run(
        const LocalGooseRunRequest& request) override;
};

} // namespace gaudere_agent

#endif
