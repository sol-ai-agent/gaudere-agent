#ifndef GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_STORE_HPP
#define GAUDERE_AGENT_LOCAL_ACTIVITY_PULSE_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr const char* local_activity_pulse_scope =
    "continuity.local-observation-pulse.v1";
inline constexpr int local_activity_pulse_sidecar_schema = 1;
inline constexpr std::int64_t local_activity_pulse_cadence_ms = 86'400'000;

enum class LocalActivityPulseState {
    idle = 0,
    preparing = 1,
    settled = 2,
    blocked = 3,
    quiescent = 4
};

struct LocalActivityPulseCursor {
    std::string scope = local_activity_pulse_scope;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    LocalActivityPulseState state = LocalActivityPulseState::idle;
    std::string anchor_checkpoint_task_id;
    std::string anchor_checkpoint_result_sha256;
    std::int64_t anchor_at_ms = 0;
    std::int64_t due_at_ms = 0;
    std::optional<std::int64_t> captured_at_ms;
    std::string task_id;
    std::optional<std::string> result_sha256;
    std::optional<std::string> predecessor_observation_task_id;
    std::optional<std::string> predecessor_observation_result_sha256;
    std::string blocked_reason;
};

[[nodiscard]] bool valid_local_activity_pulse_cursor(
    const LocalActivityPulseCursor& cursor) noexcept;

[[nodiscard]] bool valid_local_activity_pulse_transition(
    const LocalActivityPulseCursor& expected,
    const LocalActivityPulseCursor& replacement) noexcept;

struct LocalActivityPulseSidecarInspection {
    bool eligible = false;
    std::optional<LocalActivityPulseCursor> cursor;
    std::string detail;
};

/** Strict read-only inspection of one already-existing sidecar. */
[[nodiscard]] LocalActivityPulseSidecarInspection
inspect_local_activity_pulse_sidecar(const std::string& path) noexcept;

enum class LocalActivityPulseStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalActivityPulseStoreWrite {
    LocalActivityPulseStoreResult result = LocalActivityPulseStoreResult::invalid;
    std::optional<LocalActivityPulseCursor> cursor;
    std::string detail;
};

/**
 * Agent-owned durable local-activity cursor.
 *
 * The sidecar is separate from the Core state DB and contains no provider,
 * Action, WakeIntent, secret, network or host-control authority. Writes are CAS
 * by durable revision. Anchor checkpoint identity is immutable after seeding.
 */
class LocalActivityPulseStore {
public:
    explicit LocalActivityPulseStore(const std::string& path);
    ~LocalActivityPulseStore();

    LocalActivityPulseStore(const LocalActivityPulseStore&) = delete;
    LocalActivityPulseStore& operator=(const LocalActivityPulseStore&) = delete;

    [[nodiscard]] std::optional<LocalActivityPulseCursor> find(
        const std::string& scope) const;

    [[nodiscard]] LocalActivityPulseStoreWrite seed(
        const LocalActivityPulseCursor& cursor);

    [[nodiscard]] LocalActivityPulseStoreWrite replace(
        const LocalActivityPulseCursor& expected,
        const LocalActivityPulseCursor& replacement);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
