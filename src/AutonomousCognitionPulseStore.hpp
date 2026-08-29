#ifndef GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_STORE_HPP
#define GAUDERE_AGENT_AUTONOMOUS_COGNITION_PULSE_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr const char* autonomous_cognition_pulse_scope =
    "cognition.autonomous-pulse.v0";
inline constexpr int autonomous_cognition_pulse_sidecar_schema = 1;

enum class AutonomousCognitionPulseState {
    idle = 0,
    preparing = 1,
    prepared = 2,
    blocked = 3,
    quiescent = 4
};

struct AutonomousCognitionPulseCursor {
    std::string scope = autonomous_cognition_pulse_scope;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    AutonomousCognitionPulseState state = AutonomousCognitionPulseState::idle;
    std::string predecessor_task_id;
    std::string predecessor_result_sha256;
    std::int64_t anchor_at_ms = 0;
    std::int64_t due_at_ms = 0;
    std::optional<std::int64_t> observed_at_ms;
    std::string snapshot_task_id;
    std::string current_task_id;
    std::string blocked_reason;
};

[[nodiscard]] bool valid_autonomous_cognition_pulse_cursor(
    const AutonomousCognitionPulseCursor& cursor) noexcept;

struct AutonomousCognitionPulseSidecarInspection {
    bool eligible = false;
    std::optional<AutonomousCognitionPulseCursor> cursor;
    std::string detail;
};

/**
 * Inspect an existing sidecar strictly read-only.
 *
 * The file must already exist, be schema v1, contain exactly one canonical fixed-
 * scope cursor and contain no extra cursor rows. No schema creation, WAL activation
 * or durable mutation occurs through this function.
 */
[[nodiscard]] AutonomousCognitionPulseSidecarInspection
inspect_autonomous_cognition_pulse_sidecar(const std::string& path) noexcept;

enum class AutonomousCognitionPulseStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct AutonomousCognitionPulseStoreWrite {
    AutonomousCognitionPulseStoreResult result =
        AutonomousCognitionPulseStoreResult::invalid;
    std::optional<AutonomousCognitionPulseCursor> cursor;
    std::string detail;
};

/**
 * Agent-owned durable cursor sidecar.
 *
 * This store is deliberately separate from the generic Gaudere Core state DB.
 * It contains no Task, Action, budget, WakeIntent, Provider or secret state.
 * replace() is compare-and-swap by durable revision so stale writers fail closed.
 */
class AutonomousCognitionPulseStore {
public:
    explicit AutonomousCognitionPulseStore(const std::string& path);
    ~AutonomousCognitionPulseStore();

    AutonomousCognitionPulseStore(const AutonomousCognitionPulseStore&) = delete;
    AutonomousCognitionPulseStore& operator=(
        const AutonomousCognitionPulseStore&) = delete;

    [[nodiscard]] std::optional<AutonomousCognitionPulseCursor> find(
        const std::string& scope) const;

    [[nodiscard]] AutonomousCognitionPulseStoreWrite seed(
        const AutonomousCognitionPulseCursor& cursor);

    [[nodiscard]] AutonomousCognitionPulseStoreWrite replace(
        const AutonomousCognitionPulseCursor& expected,
        const AutonomousCognitionPulseCursor& replacement);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
