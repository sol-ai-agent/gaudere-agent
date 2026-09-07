#ifndef GAUDERE_AGENT_GOOSE_GOVERNANCE_STORE_HPP
#define GAUDERE_AGENT_GOOSE_GOVERNANCE_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace gaudere_agent {

struct GooseOperationalPolicy {
    std::uint64_t revision = 0;
    std::vector<std::string> enabled_tools;
    std::size_t max_calls_per_cycle = 8;
};

struct GooseRiskProposal {
    std::string id;
    std::string proposal;
    std::string reason;
    std::string status;
    std::optional<std::string> review_task_id;
};

/**
 * Durable governance state owned by Gaudere's local Goose surface.
 *
 * Operational policy may vary inside the compiled current risk envelope.
 * Risk proposals are append-only pending review records. This store deliberately
 * has no API that can approve or promote a risk proposal locally: promotion is
 * reserved for a future OpenAI-validated Gaudere governance path.
 */
class GooseGovernanceStore {
public:
    explicit GooseGovernanceStore(std::string path);
    ~GooseGovernanceStore();

    GooseGovernanceStore(const GooseGovernanceStore&) = delete;
    GooseGovernanceStore& operator=(const GooseGovernanceStore&) = delete;

    [[nodiscard]] GooseOperationalPolicy operational_policy() const;
    [[nodiscard]] GooseOperationalPolicy set_operational_policy(
        const std::vector<std::string>& enabled_tools,
        std::size_t max_calls_per_cycle);

    [[nodiscard]] GooseRiskProposal propose_risk_rule_change(
        const std::string& proposal,
        const std::string& reason);
    [[nodiscard]] std::optional<GooseRiskProposal> find_risk_proposal(
        const std::string& id) const;
    [[nodiscard]] std::vector<GooseRiskProposal> list_risk_proposals() const;
    [[nodiscard]] bool mark_risk_review_submitted(
        const std::string& id,
        const std::string& review_task_id);

    [[nodiscard]] static const std::vector<std::string>& runtime_tool_envelope();
    [[nodiscard]] static std::size_t risk_max_calls_per_cycle() noexcept;

private:
    void initialize();

    std::string path_;
    sqlite3* db_ = nullptr;
};

} // namespace gaudere_agent

#endif
