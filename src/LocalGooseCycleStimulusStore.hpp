#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_STORE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_STIMULUS_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace gaudere_agent {

inline constexpr const char* local_goose_cycle_stimulus_scope =
    "cognition.local-goose-cycle.stimulus.v1";
inline constexpr const char* local_goose_cycle_explicit_recheck_source =
    "explicit_local_recheck";
inline constexpr const char* local_goose_cycle_stimulus_id_prefix =
    "cognition.local-goose-cycle.stimulus.v1:";
inline constexpr int local_goose_cycle_stimulus_sidecar_schema = 1;

enum class LocalGooseCycleStimulusStatus {
    accepted = 0,
    consumed = 1,
    superseded = 2,
    manual_review = 3
};

struct LocalGooseCycleStimulus {
    std::string scope = local_goose_cycle_stimulus_scope;
    std::string id;
    std::string source_kind = local_goose_cycle_explicit_recheck_source;
    std::string source_id;
    std::int64_t accepted_at_ms = 0;
    std::uint64_t target_cycle_revision = 0;
    std::uint64_t target_cycle_generation = 0;
    LocalGooseCycleStimulusStatus status =
        LocalGooseCycleStimulusStatus::accepted;
    std::optional<std::int64_t> terminal_at_ms;
    std::optional<std::uint64_t> resulting_cycle_revision;
    std::string terminal_reason;
};

[[nodiscard]] LocalGooseCycleStimulus make_explicit_local_recheck_stimulus(
    const std::string& source_id,
    std::int64_t accepted_at_ms,
    std::uint64_t target_cycle_revision,
    std::uint64_t target_cycle_generation);

[[nodiscard]] bool valid_local_goose_cycle_stimulus(
    const LocalGooseCycleStimulus& stimulus) noexcept;

[[nodiscard]] bool valid_local_goose_cycle_stimulus_transition(
    const LocalGooseCycleStimulus& expected,
    const LocalGooseCycleStimulus& replacement) noexcept;

enum class LocalGooseCycleStimulusStoreResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseCycleStimulusStoreWrite {
    LocalGooseCycleStimulusStoreResult result =
        LocalGooseCycleStimulusStoreResult::invalid;
    std::optional<LocalGooseCycleStimulus> stimulus;
    std::string detail;
};

struct LocalGooseCycleStimulusSidecarInspection {
    bool eligible = false;
    std::vector<LocalGooseCycleStimulus> stimuli;
    std::string detail;
};

[[nodiscard]] LocalGooseCycleStimulusSidecarInspection
inspect_local_goose_cycle_stimulus_sidecar(const std::string& path) noexcept;

class LocalGooseCycleStimulusStore {
public:
    explicit LocalGooseCycleStimulusStore(const std::string& path);
    ~LocalGooseCycleStimulusStore();

    LocalGooseCycleStimulusStore(
        const LocalGooseCycleStimulusStore&) = delete;
    LocalGooseCycleStimulusStore& operator=(
        const LocalGooseCycleStimulusStore&) = delete;

    [[nodiscard]] std::optional<LocalGooseCycleStimulus> find(
        const std::string& id) const;

    [[nodiscard]] std::optional<LocalGooseCycleStimulus> find_by_source(
        const std::string& source_id) const;

    [[nodiscard]] std::optional<LocalGooseCycleStimulus>
    find_accepted_for_target(std::uint64_t target_cycle_revision) const;

    [[nodiscard]] LocalGooseCycleStimulusStoreWrite append(
        const LocalGooseCycleStimulus& stimulus);

    [[nodiscard]] LocalGooseCycleStimulusStoreWrite replace(
        const LocalGooseCycleStimulus& expected,
        const LocalGooseCycleStimulus& replacement);

private:
    sqlite3* database_ = nullptr;
};

} // namespace gaudere_agent

#endif
