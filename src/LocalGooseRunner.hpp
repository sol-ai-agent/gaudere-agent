#ifndef GAUDERE_AGENT_LOCAL_GOOSE_RUNNER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_RUNNER_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace gaudere_agent {

enum class LocalGooseRunOutcome { succeeded, failed, timed_out, output_too_large };

struct LocalGooseRunRequest {
    std::string model_path;
    std::string prompt;
    std::chrono::milliseconds timeout{std::chrono::minutes{10}};
    std::size_t max_output_bytes = 16 * 1024;

    // When enabled, Goose receives only Gaudere's fixed typed MCP extension.
    // The extension itself obtains its runtime authority from Gaudere's durable
    // operational policy and current risk envelope.
    bool tools_enabled = false;
    std::string control_socket;
    std::string governance_path;
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

/**
 * Fixed, no-shell Goose invocation. The production container remains Network=none.
 *
 * An optional cooperative pump lets the sole Gaudere runtime thread service typed
 * MCP requests while the child Goose process is alive. The pump is never executed
 * by the child and does not grant Goose any authority by itself.
 */
class PosixLocalGooseRunner final : public LocalGooseRunner {
public:
    using CooperativePump = std::function<void()>;

    explicit PosixLocalGooseRunner(CooperativePump cooperative_pump = {});

    [[nodiscard]] LocalGooseRunResult run(
        const LocalGooseRunRequest& request) override;

private:
    CooperativePump cooperative_pump_;
};

} // namespace gaudere_agent

#endif
