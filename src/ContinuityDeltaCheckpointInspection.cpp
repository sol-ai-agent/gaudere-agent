#include "ContinuityDeltaCheckpointInspection.hpp"

#include "CanonicalCognitionDecision.hpp"
#include "ContinuityDeltaCheckpoint.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using TaskStatus = gaudere::work::TaskStatus;

constexpr std::size_t max_checkpoint_bytes = 32 * 1024;
constexpr const char* cognition_prefix = "cognition.current.v0:";
constexpr const char* snapshot_prefix = "continuity.resume-context-snapshot.v1:";
constexpr const char* provider_prefix = "provider.call:openai.responses:";
constexpr const char* provider_scope = "provider.call:openai.responses";
constexpr const char* wake_scope = "cognition.reflect.wake.v0";
constexpr const char* reconciliation_statement =
    "The durable audited context already includes the confirmed predecessor provider effect; older provider totals that omit that effect remain historical evidence and are superseded for current accounting.";

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& value, const std::string& prefix) noexcept
{
    return value.size() == prefix.size() + 64
        && value.compare(0, prefix.size(), prefix) == 0
        && lowercase_sha256(value.substr(prefix.size()));
}

bool exact_keys(const Json& value, const std::set<std::string>& expected)
{
    if (!value.is_object() || value.size() != expected.size()) return false;
    for (const auto& key : expected)
        if (!value.contains(key)) return false;
    return true;
}

bool nonnegative_u64(const Json& value, const char* key, std::uint64_t& output)
{
    if (!value.contains(key)) return false;
    const auto& field = value.at(key);
    if (field.is_number_unsigned()) {
        output = field.get<std::uint64_t>();
        return true;
    }
    if (!field.is_number_integer()) return false;
    const auto signed_value = field.get<std::int64_t>();
    if (signed_value < 0) return false;
    output = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool canonical_provider_action(const Json& action,
                               const std::string& task_id,
                               std::string& action_id,
                               std::string& action_key)
{
    static const std::set<std::string> keys = {
        "critical", "effect", "id", "idempotency_key", "status"
    };
    if (!exact_keys(action, keys)
        || !action.at("id").is_string()
        || !action.at("idempotency_key").is_string()
        || !action.at("critical").is_boolean()
        || action.value("status", "") != "succeeded"
        || action.value("effect", "") != "confirmed") {
        return false;
    }
    action_id = action.at("id").get<std::string>();
    action_key = action.at("idempotency_key").get<std::string>();
    const auto expected = std::string{provider_prefix} + task_id;
    return action_id == expected && action_key == expected;
}

bool canonical_historical_wake(const Json& wake,
                               std::string& canonical,
                               std::string& hash)
{
    if (!wake.is_object() || wake.value("scope", "") != wake_scope
        || !wake.contains("cardinality") || !wake.at("cardinality").is_string()) {
        return false;
    }
    const auto cardinality = wake.at("cardinality").get<std::string>();
    if (cardinality == "empty") {
        static const std::set<std::string> empty_keys = {"cardinality", "scope"};
        if (!exact_keys(wake, empty_keys)) return false;
    } else if (cardinality == "one") {
        static const std::set<std::string> one_keys = {
            "accepted_at_ms", "cardinality", "due_at_ms", "id", "scope",
            "source_id", "status"
        };
        if (!exact_keys(wake, one_keys)
            || !wake.at("id").is_string()
            || wake.at("id").get<std::string>().empty()
            || !wake.at("source_id").is_string()
            || wake.at("source_id").get<std::string>().empty()
            || !wake.at("status").is_string()) {
            return false;
        }
        const auto status = wake.at("status").get<std::string>();
        if (status != "scheduled" && status != "fired"
            && status != "revoked" && status != "manual_review") {
            return false;
        }
        std::uint64_t accepted = 0;
        std::uint64_t due = 0;
        if (!nonnegative_u64(wake, "accepted_at_ms", accepted)
            || !nonnegative_u64(wake, "due_at_ms", due)
            || due < accepted) {
            return false;
        }
    } else {
        return false;
    }
    canonical = wake.dump();
    hash = sha256_hex(canonical);
    return true;
}

bool canonical_limits(const gaudere::work::ResourceLimits& limits) noexcept
{
    return limits.max_input_bytes == max_checkpoint_bytes
        && limits.max_output_bytes == max_checkpoint_bytes
        && limits.max_runtime == std::chrono::seconds{2}
        && limits.max_attempts == 2;
}

} // namespace

ContinuityDeltaCheckpointInspection inspect_succeeded_continuity_delta_checkpoint(
    const gaudere::work::Task& task) noexcept
{
    ContinuityDeltaCheckpointInspection inspection;
    try {
        if (task.status != TaskStatus::succeeded || !task.result
            || task.kind != continuity_delta_checkpoint_task_kind
            || task.input_content_type != continuity_delta_checkpoint_content_type
            || task.result->content_type != continuity_delta_checkpoint_content_type
            || task.result->output != task.input
            || !task.result->failure_code.empty()
            || !task.result->failure_message.empty()
            || task.input.empty() || task.input.size() > max_checkpoint_bytes
            || !canonical_limits(task.limits)) {
            inspection.detail = "checkpoint Task/result is not canonical succeeded v1";
            return inspection;
        }

        const auto root = Json::parse(task.input);
        static const std::set<std::string> root_keys = {
            "audited", "audited_context", "current_provider_budget",
            "predecessor", "reconciliation", "schema", "unresolved_external"
        };
        if (!exact_keys(root, root_keys)
            || root.value("schema", "") != continuity_delta_checkpoint_schema
            || root.dump() != task.input) {
            inspection.detail = "checkpoint payload root is not canonical v1";
            return inspection;
        }

        static const std::set<std::string> cognition_keys = {
            "decision", "provider_action", "result_sha256", "task_id"
        };
        const auto& audited = root.at("audited");
        const auto& predecessor = root.at("predecessor");
        if (!exact_keys(audited, cognition_keys)
            || !exact_keys(predecessor, cognition_keys)
            || !audited.at("task_id").is_string()
            || !predecessor.at("task_id").is_string()
            || !audited.at("result_sha256").is_string()
            || !predecessor.at("result_sha256").is_string()
            || !audited.at("decision").is_object()
            || !predecessor.at("decision").is_object()) {
            inspection.detail = "checkpoint cognition evidence is malformed";
            return inspection;
        }
        inspection.audited_task_id = audited.at("task_id").get<std::string>();
        inspection.predecessor_task_id = predecessor.at("task_id").get<std::string>();
        if (!prefixed_sha256(inspection.audited_task_id, cognition_prefix)
            || !prefixed_sha256(inspection.predecessor_task_id, cognition_prefix)
            || inspection.audited_task_id == inspection.predecessor_task_id
            || !lowercase_sha256(audited.at("result_sha256").get<std::string>())
            || !lowercase_sha256(predecessor.at("result_sha256").get<std::string>())) {
            inspection.detail = "checkpoint cognition identities are non-canonical";
            return inspection;
        }
        const auto audited_decision =
            inspect_canonical_cognition_decision(audited.at("decision").dump());
        const auto predecessor_decision =
            inspect_canonical_cognition_decision(predecessor.at("decision").dump());
        if (!audited_decision.eligible || !predecessor_decision.eligible) {
            inspection.detail = "checkpoint cognition decisions are non-canonical";
            return inspection;
        }
        if (!canonical_provider_action(audited.at("provider_action"),
                                       inspection.audited_task_id,
                                       inspection.audited_provider_action_id,
                                       inspection.audited_provider_action_key)
            || !canonical_provider_action(predecessor.at("provider_action"),
                                          inspection.predecessor_task_id,
                                          inspection.predecessor_provider_action_id,
                                          inspection.predecessor_provider_action_key)) {
            inspection.detail = "checkpoint provider Action evidence is non-canonical";
            return inspection;
        }

        static const std::set<std::string> context_keys = {
            "captured_at_ms", "historical_wake", "provider_budget_scope",
            "provider_total_before", "snapshot_task_id"
        };
        const auto& context = root.at("audited_context");
        std::uint64_t captured_at = 0;
        if (!exact_keys(context, context_keys)
            || context.value("provider_budget_scope", "") != provider_scope
            || !context.at("snapshot_task_id").is_string()
            || !prefixed_sha256(context.at("snapshot_task_id").get<std::string>(),
                                snapshot_prefix)
            || !nonnegative_u64(context, "captured_at_ms", captured_at)) {
            inspection.detail = "checkpoint audited context is non-canonical";
            return inspection;
        }
        std::uint64_t total_before = 0;
        if (!nonnegative_u64(context, "provider_total_before", total_before)) {
            inspection.detail = "checkpoint provider total-before is invalid";
            return inspection;
        }

        static const std::set<std::string> budget_keys = {"scope", "total_used"};
        const auto& budget = root.at("current_provider_budget");
        if (!exact_keys(budget, budget_keys)
            || budget.value("scope", "") != provider_scope
            || !nonnegative_u64(budget, "total_used", inspection.provider_total)
            || total_before == std::numeric_limits<std::uint64_t>::max()
            || inspection.provider_total != total_before + 1U) {
            inspection.detail = "checkpoint current provider budget is non-canonical";
            return inspection;
        }
        inspection.provider_scope = provider_scope;

        if (!canonical_historical_wake(context.at("historical_wake"),
                                       inspection.historical_wake_canonical,
                                       inspection.historical_wake_sha256)) {
            inspection.detail = "checkpoint historical WakeIntent evidence is non-canonical";
            return inspection;
        }
        inspection.historical_wake_scope = wake_scope;

        static const std::set<std::string> reconciliation_keys = {
            "audited_provider_effect_confirmed",
            "predecessor_provider_effect_confirmed",
            "provider_increment_from_audited_context", "statement"
        };
        const auto& reconciliation = root.at("reconciliation");
        if (!exact_keys(reconciliation, reconciliation_keys)
            || reconciliation.value("provider_increment_from_audited_context", 0) != 1
            || !reconciliation.value("predecessor_provider_effect_confirmed", false)
            || !reconciliation.value("audited_provider_effect_confirmed", false)
            || reconciliation.value("statement", "") != reconciliation_statement) {
            inspection.detail = "checkpoint reconciliation evidence is non-canonical";
            return inspection;
        }

        const auto& unresolved = root.at("unresolved_external");
        if (!unresolved.is_array() || unresolved.size() != 3
            || unresolved[0] != "external_checkpoint_identity"
            || unresolved[1] != "rollback_reference"
            || unresolved[2] != "stopped_state_backup_marker") {
            inspection.detail = "checkpoint unresolved-external marker set is non-canonical";
            return inspection;
        }

        const auto expected_id = std::string{continuity_delta_checkpoint_task_prefix}
            + sha256_hex(task.input);
        const auto expected_key = std::string{continuity_delta_checkpoint_task_prefix}
            + "audited:" + inspection.audited_task_id;
        if (task.id != expected_id || task.idempotency_key != expected_key) {
            inspection.detail = "checkpoint Task identity does not match canonical payload";
            return inspection;
        }

        inspection.checkpoint_result_sha256 = sha256_hex(task.result->output);
        inspection.eligible = true;
        return inspection;
    } catch (const std::exception& error) {
        inspection.detail = error.what();
        return inspection;
    } catch (...) {
        inspection.detail = "checkpoint inspection failed";
        return inspection;
    }
}

} // namespace gaudere_agent
