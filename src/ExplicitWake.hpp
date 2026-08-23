#ifndef GAUDERE_AGENT_EXPLICIT_WAKE_HPP
#define GAUDERE_AGENT_EXPLICIT_WAKE_HPP

#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr const char* explicit_wake_scope =
    "cognition.reflect.wake.v0";
inline constexpr std::uint64_t explicit_wake_max_total = 1;

enum class ExplicitWakeAcceptResult {
    accepted,
    duplicate,
    source_not_found,
    source_ineligible,
    total_exhausted,
    conflict,
    invalid
};

struct ExplicitWakeAcceptance {
    ExplicitWakeAcceptResult result = ExplicitWakeAcceptResult::invalid;
    std::optional<gaudere::scheduling::wake::WakeIntent> intent;
    std::string detail;
};

struct ExplicitWakeStatus {
    bool healthy = false;
    std::string report;
};

/** Explicit authority boundary from one canonical reflection Task to one wake.
 *
 * The application-fixed scope and lifetime maximum are verified at construction.
 * Model output can supply only the already-normalized bounded delay; it cannot
 * select scope, identity, policy, an absolute timestamp, or any follow-up action.
 */
class ExplicitWake {
public:
    ExplicitWake(gaudere::work::TaskStore& task_store,
                 gaudere::scheduling::wake::WakeIntentRuntime& wake_runtime);

    [[nodiscard]] ExplicitWakeAcceptance accept(
        const std::string& source_task_id);
    [[nodiscard]] gaudere::scheduling::wake::WakeIntentRevokeResult revoke(
        const std::string& wake_id,
        const std::string& reason);
    [[nodiscard]] std::optional<gaudere::scheduling::wake::WakeIntent> find(
        const std::string& wake_id) const;

    /** Read-only composite status for the application-fixed wake scope.
     *
     * The caller supplies already-observed worker lease and scheduler deadlines.
     * This method never reconciles, refreshes, accepts, revokes, submits work, or
     * performs provider effects. Source eligibility reuses the exact canonical
     * decision validator used by accept().
     */
    [[nodiscard]] ExplicitWakeStatus inspect_status(
        std::optional<gaudere::scheduling::wake::WakeIntentTimePoint>
            next_lease_at,
        std::optional<gaudere::scheduling::wake::WakeIntentTimePoint>
            scheduler_next_at) const;

private:
    gaudere::work::TaskStore& task_store_;
    gaudere::scheduling::wake::WakeIntentRuntime& wake_runtime_;
};

[[nodiscard]] std::string wake_intent_report(
    const gaudere::scheduling::wake::WakeIntent& intent);

} // namespace gaudere_agent

#endif
