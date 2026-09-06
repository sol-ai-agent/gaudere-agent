#include "ContinuityDeltaCheckpoint.hpp"
#include "ContinuityDeltaCheckpointInspection.hpp"
#include "Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string repeated(const char value)
{
    return std::string(64, value);
}

std::string cognition(const char value)
{
    return "cognition.current.v0:" + repeated(value);
}

std::string snapshot(const char value)
{
    return "continuity.resume-context-snapshot.v1:" + repeated(value);
}

nlohmann::json decision(const std::string& reason)
{
    return nlohmann::json{
        {"schema", "gaudere.cognition.resume-decision.v1"},
        {"decision", "stop"},
        {"reason", reason}
    };
}

nlohmann::json action(const std::string& task_id)
{
    const auto identity = "provider.call:openai.responses:" + task_id;
    return nlohmann::json{
        {"id", identity},
        {"idempotency_key", identity},
        {"status", "succeeded"},
        {"effect", "confirmed"},
        {"critical", true}
    };
}

nlohmann::json payload()
{
    const auto audited = cognition('a');
    const auto predecessor = cognition('b');
    return nlohmann::json{
        {"schema", gaudere_agent::continuity_delta_checkpoint_schema},
        {"audited", nlohmann::json{
            {"task_id", audited},
            {"result_sha256", repeated('c')},
            {"decision", decision("synthetic audited checkpoint evidence")},
            {"provider_action", action(audited)}
        }},
        {"predecessor", nlohmann::json{
            {"task_id", predecessor},
            {"result_sha256", repeated('d')},
            {"decision", decision("synthetic predecessor checkpoint evidence")},
            {"provider_action", action(predecessor)}
        }},
        {"audited_context", nlohmann::json{
            {"snapshot_task_id", snapshot('e')},
            {"captured_at_ms", 1000},
            {"provider_budget_scope", "provider.call:openai.responses"},
            {"provider_total_before", 9},
            {"historical_wake", nlohmann::json{
                {"scope", "cognition.reflect.wake.v0"},
                {"cardinality", "one"},
                {"id", "wake.synthetic"},
                {"source_id", "source.synthetic"},
                {"status", "fired"},
                {"accepted_at_ms", 100},
                {"due_at_ms", 200}
            }}
        }},
        {"current_provider_budget", nlohmann::json{
            {"scope", "provider.call:openai.responses"},
            {"total_used", 10}
        }},
        {"reconciliation", nlohmann::json{
            {"provider_increment_from_audited_context", 1},
            {"predecessor_provider_effect_confirmed", true},
            {"audited_provider_effect_confirmed", true},
            {"statement", "The durable audited context already includes the confirmed predecessor provider effect; older provider totals that omit that effect remain historical evidence and are superseded for current accounting."}
        }},
        {"unresolved_external", nlohmann::json::array({
            "external_checkpoint_identity",
            "rollback_reference",
            "stopped_state_backup_marker"
        })}
    };
}

gaudere::work::Task checkpoint_task(const nlohmann::json& value)
{
    using namespace gaudere_agent;
    gaudere::work::Task task;
    task.input = value.dump();
    task.id = std::string{continuity_delta_checkpoint_task_prefix}
        + sha256_hex(task.input);
    task.idempotency_key = std::string{continuity_delta_checkpoint_task_prefix}
        + "audited:" + value.at("audited").at("task_id").get<std::string>();
    task.kind = continuity_delta_checkpoint_task_kind;
    task.input_content_type = continuity_delta_checkpoint_content_type;
    task.limits.max_input_bytes = 32 * 1024;
    task.limits.max_output_bytes = 32 * 1024;
    task.limits.max_runtime = std::chrono::seconds{2};
    task.limits.max_attempts = 2;
    task.status = gaudere::work::TaskStatus::succeeded;
    task.result = gaudere::work::TaskResult{
        continuity_delta_checkpoint_content_type, task.input, {}, {}};
    return task;
}

} // namespace

int main()
{
    using namespace gaudere_agent;

    const auto canonical_payload = payload();
    const auto task = checkpoint_task(canonical_payload);
    const auto inspection = inspect_succeeded_continuity_delta_checkpoint(task);
    expect(inspection.eligible,
           "canonical succeeded checkpoint is accepted read-only");
    expect(inspection.audited_task_id == cognition('a')
               && inspection.predecessor_task_id == cognition('b')
               && inspection.provider_scope == "provider.call:openai.responses"
               && inspection.provider_total == 10,
           "bounded checkpoint identities and provider evidence are extracted");
    expect(inspection.audited_provider_action_id
               == "provider.call:openai.responses:" + cognition('a')
               && inspection.audited_provider_action_key
               == inspection.audited_provider_action_id
               && inspection.predecessor_provider_action_id
               == "provider.call:openai.responses:" + cognition('b')
               && inspection.predecessor_provider_action_key
               == inspection.predecessor_provider_action_id,
           "exact provider Action identities and keys are extracted");
    expect(inspection.historical_wake_scope == "cognition.reflect.wake.v0"
               && inspection.historical_wake_sha256.size() == 64
               && !inspection.historical_wake_canonical.empty(),
           "historical scoped WakeIntent evidence is canonicalized and hashed");
    expect(inspection.checkpoint_result_sha256 == sha256_hex(task.input),
           "checkpoint echoed result hash is exposed as immutable anchor evidence");

    auto bad_effect_payload = canonical_payload;
    bad_effect_payload["audited"]["provider_action"]["effect"] = "unknown";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_effect_payload)).eligible,
           "non-confirmed provider Action evidence is rejected");

    auto bad_action_key_payload = canonical_payload;
    bad_action_key_payload["audited"]["provider_action"]["idempotency_key"] =
        "provider.call:openai.responses:other";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_action_key_payload)).eligible,
           "provider Action idempotency key must match exact current-cognition identity");

    auto bad_decision_payload = canonical_payload;
    bad_decision_payload["audited"]["decision"]["schema"] = "decision.synthetic";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_decision_payload)).eligible,
           "non-canonical cognition decision evidence is rejected");

    auto bad_snapshot_payload = canonical_payload;
    bad_snapshot_payload["audited_context"]["snapshot_task_id"] = "snapshot.synthetic";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_snapshot_payload)).eligible,
           "snapshot evidence requires exact content-addressed snapshot identity");

    auto negative_capture_payload = canonical_payload;
    negative_capture_payload["audited_context"]["captured_at_ms"] = -1;
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(negative_capture_payload)).eligible,
           "negative audited-context capture time is rejected");

    auto extra_field_payload = canonical_payload;
    extra_field_payload["unexpected"] = true;
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(extra_field_payload)).eligible,
           "unknown top-level checkpoint fields are rejected");

    auto bad_budget_payload = canonical_payload;
    bad_budget_payload["current_provider_budget"]["total_used"] = 11;
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_budget_payload)).eligible,
           "provider total must remain audited-context total plus one");

    auto bad_wake_payload = canonical_payload;
    bad_wake_payload["audited_context"]["historical_wake"]["scope"] = "other.scope";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_wake_payload)).eligible,
           "historical WakeIntent scope cannot drift");

    auto bad_reconciliation_payload = canonical_payload;
    bad_reconciliation_payload["reconciliation"]["statement"] = "changed";
    expect(!inspect_succeeded_continuity_delta_checkpoint(
                checkpoint_task(bad_reconciliation_payload)).eligible,
           "checkpoint reconciliation statement is exact durable evidence");

    auto wrong_identity = task;
    wrong_identity.id.back() = wrong_identity.id.back() == '0' ? '1' : '0';
    expect(!inspect_succeeded_continuity_delta_checkpoint(wrong_identity).eligible,
           "checkpoint Task identity tampering is rejected");

    auto wrong_result = task;
    wrong_result.result->output += "x";
    expect(!inspect_succeeded_continuity_delta_checkpoint(wrong_result).eligible,
           "checkpoint result must echo exact canonical input");

    if (failures != 0) {
        std::cerr << failures << " checkpoint inspection test(s) failed\n";
        return 1;
    }
    std::cout << "All continuity delta checkpoint inspection tests passed\n";
    return 0;
}
