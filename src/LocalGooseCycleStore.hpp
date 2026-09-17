#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr const char* local_goose_cycle_scope =
    "cognition.local-goose-cycle.v1";
inline constexpr int local_goose_cycle_sidecar_schema = 1;

enum class LocalGooseCycleState {
    dormant = 0,
    scheduled = 1,
    prepared = 2,
    blocked = 3
};

struct LocalGooseCycleCursor {
    std::string scope = local_goose_cycle_scope;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    LocalGooseCycleState state = LocalGooseCycleState::dormant;

    std::string anchor_observation_task_id;
    std::string anchor_observation_result_sha256;

    std::optional<std::string> predecessor_task_id;
    std::optional<std::string> predecessor_result_sha256;

    std::optional<std::int64_t> due_at_ms;
    std::optional<std::int64_t> captured_at_ms;
    std::string current_task_id;
    std::string blocked_reason;
};

[[nodiscard]] bool valid_local_goose_cycle_cursor(
    const LocalGooseCycleCursor& cursor) noexcept;

[[nodiscard]] bool valid_local_goose_cycle_transition(
    const LocalGooseCycleCursor& expected,
    const LocalGooseCycleCursor& replacement) noexcept;

struct LocalGooseCycleSidecarInspection {
    bool eligible = false;
    std::optional<LocalGooseCycleCursor> cursor;
    std::string detail;
};

/** Strict read-only inspection of one already-existing sidecar. */
[[nodiscard]] LocalGooseCycleSidecarInspection
inspect_local_goose_cycle_sidecar(const std::string& path) noexcept;

enum class LocalGooseCycleStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseCycleStoreWrite {
    LocalGooseCycleStoreResult result = LocalGooseCycleStoreResult::invalid;
    std::optional<LocalGooseCycleCursor> cursor;
    std::string detail;
};

/**
 * Agent-owned durable self-scheduling Local Goose cursor.
 *
 * The sidecar is deliberately separate from both Core state.db and the historical
 * local-activity pulse sidecar. It carries no provider, Action, WakeIntent, secret,
 * network or arbitrary-command authority. Writes are compare-and-swap by revision.
 */
class LocalGooseCycleStore {
public:
    explicit LocalGooseCycleStore(const std::string& path);
    ~LocalGooseCycleStore();

    LocalGooseCycleStore(const LocalGooseCycleStore&) = delete;
    LocalGooseCycleStore& operator=(const LocalGooseCycleStore&) = delete;

    [[nodiscard]] std::optional<LocalGooseCycleCursor> find(
        const std::string& scope) const;

    [[nodiscard]] LocalGooseCycleStoreWrite seed(
        const LocalGooseCycleCursor& cursor);

    [[nodiscard]] LocalGooseCycleStoreWrite replace(
        const LocalGooseCycleCursor& expected,
        const LocalGooseCycleCursor& replacement);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
