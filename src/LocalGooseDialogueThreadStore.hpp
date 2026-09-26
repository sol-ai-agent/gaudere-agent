#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_THREAD_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_THREAD_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr int local_goose_dialogue_thread_sidecar_schema = 1;

struct LocalGooseDialogueThreadHead {
    std::string alias;
    std::uint64_t revision = 0;
    std::string root_task_id;
    std::string head_task_id;
};

[[nodiscard]] bool valid_local_goose_dialogue_thread_head(
    const LocalGooseDialogueThreadHead& head) noexcept;

[[nodiscard]] bool valid_local_goose_dialogue_thread_head_transition(
    const LocalGooseDialogueThreadHead& expected,
    const LocalGooseDialogueThreadHead& replacement) noexcept;

enum class LocalGooseDialogueThreadStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueThreadStoreWrite {
    LocalGooseDialogueThreadStoreResult result =
        LocalGooseDialogueThreadStoreResult::invalid;
    std::optional<LocalGooseDialogueThreadHead> head;
    std::string detail;
};

/**
 * Durable preferred-head coordinator for normal multi-actor dialogue UX.
 *
 * It stores only alias -> root/head/revision. It owns no model, provider,
 * scheduler, stimulus, shell, network, or Task execution authority.
 *
 * Writes use compare-and-swap by revision. The immutable dialogue Task lineage
 * remains authoritative evidence; this sidecar merely selects one preferred
 * branch for ordinary serialized interaction.
 */
class LocalGooseDialogueThreadStore {
public:
    explicit LocalGooseDialogueThreadStore(const std::string& path);
    ~LocalGooseDialogueThreadStore();

    LocalGooseDialogueThreadStore(const LocalGooseDialogueThreadStore&) = delete;
    LocalGooseDialogueThreadStore& operator=(
        const LocalGooseDialogueThreadStore&) = delete;

    [[nodiscard]] std::optional<LocalGooseDialogueThreadHead> find(
        const std::string& alias) const;

    [[nodiscard]] LocalGooseDialogueThreadStoreWrite seed(
        const LocalGooseDialogueThreadHead& head);

    [[nodiscard]] LocalGooseDialogueThreadStoreWrite replace(
        const LocalGooseDialogueThreadHead& expected,
        const LocalGooseDialogueThreadHead& replacement);

private:
    sqlite3* database_ = nullptr;
};

[[nodiscard]] std::string local_goose_dialogue_thread_head_report(
    const LocalGooseDialogueThreadHead& head);

} // namespace gaudere_agent

#endif
