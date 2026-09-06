#include "LocalContinuityObservation.hpp"

#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

constexpr std::size_t max_observation_bytes = 32 * 1024;
constexpr std::uint32_t max_generation = 3;
constexpr const char* checkpoint_prefix = "continuity.delta-checkpoint.v1:";

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& value, const std::string& prefix) noexcept
{
    return value.size() == prefix.size() + 64
        && value.compare(0, prefix.size(), prefix) == 0
        && lowercase_sha256(value.substr(prefix.size()));
}

bool same_limits(const gaudere::work::ResourceLimits& a,
                 const gaudere::work::ResourceLimits& b) noexcept
{
    return a.max_input_bytes == b.max_input_bytes
        && a.max_output_bytes == b.max_output_bytes
        && a.max_runtime == b.max_runtime
        && a.max_attempts == b.max_attempts;
}

bool same_definition(const Task& a, const Task& b) noexcept
{
    return a.id == b.id
        && a.idempotency_key == b.idempotency_key
        && a.kind == b.kind
        && a.input_content_type == b.input_content_type
        && a.input == b.input
        && same_limits(a.limits, b.limits);
}

bool unsigned_field(const Json& value, const char* name, std::uint64_t& out)
{
    if (!value.contains(name)) return false;
    const auto& field = value.at(name);
    if (field.is_number_unsigned()) {
        out = field.get<std::uint64_t>();
        return true;
    }
    if (!field.is_number_integer()) return false;
    const auto signed_value = field.get<std::int64_t>();
    if (signed_value < 0) return false;
    out = static_cast<std::uint64_t>(signed_value);
    return true;
}

bool signed_nonnegative_field(const Json& value, const char* name, std::int64_t& out)
{
    if (!value.contains(name) || !value.at(name).is_number_integer()) return false;
    out = value.at(name).get<std::int64_t>();
    return out >= 0;
}

bool nullable_string_field(const Json& value,
                           const char* name,
                           std::optional<std::string>& out)
{
    if (!value.contains(name)) return false;
    const auto& field = value.at(name);
    if (field.is_null()) {
        out.reset();
        return true;
    }
    if (!field.is_string()) return false;
    out = field.get<std::string>();
    return true;
}

Json nullable_string_json(const std::optional<std::string>& value)
{
    return value ? Json(*value) : Json(nullptr);
}

Json payload_json(const LocalContinuityObservationFacts& facts)
{
    return Json{
        {"actions_confirmed", facts.actions_confirmed},
        {"actions_total", facts.actions_total},
        {"anchor_checkpoint_task_id", facts.anchor_checkpoint_task_id},
        {"captured_at_ms", facts.captured_at_ms},
        {"checkpoint_count", facts.checkpoint_count},
        {"due_at_ms", facts.due_at_ms},
        {"generation", facts.generation},
        {"lateness_ms", facts.captured_at_ms - facts.due_at_ms},
        {"latest_checkpoint_task_id", facts.latest_checkpoint_task_id},
        {"predecessor_observation_result_sha256",
         nullable_string_json(facts.predecessor_observation_result_sha256)},
        {"predecessor_observation_task_id",
         nullable_string_json(facts.predecessor_observation_task_id)},
        {"provider_limit", facts.provider_limit},
        {"provider_total", facts.provider_total},
        {"schema", local_continuity_observation_schema},
        {"scope", local_continuity_observation_scope},
        {"wake_fired", facts.wake_fired},
        {"wake_total", facts.wake_total}
    };
}

bool validate_facts(const LocalContinuityObservationFacts& facts,
                    std::string& detail) noexcept
{
    if (facts.generation == 0 || facts.generation > max_generation) {
        detail = "generation is outside bounded range 1..3";
        return false;
    }
    if (facts.due_at_ms < 0 || facts.captured_at_ms < facts.due_at_ms) {
        detail = "capture time precedes durable due time";
        return false;
    }
    if (!prefixed_sha256(facts.anchor_checkpoint_task_id, checkpoint_prefix)) {
        detail = "anchor checkpoint identity is not canonical";
        return false;
    }
    if (facts.checkpoint_count != 1
        || facts.latest_checkpoint_task_id != facts.anchor_checkpoint_task_id) {
        detail = "checkpoint continuity is not exactly the anchored checkpoint";
        return false;
    }
    if (facts.provider_limit == 0 || facts.provider_total > facts.provider_limit) {
        detail = "provider budget snapshot is invalid";
        return false;
    }
    if (facts.actions_confirmed > facts.actions_total) {
        detail = "confirmed Action count exceeds total";
        return false;
    }
    if (facts.wake_fired > facts.wake_total) {
        detail = "fired WakeIntent count exceeds total";
        return false;
    }

    if (facts.generation == 1) {
        if (facts.predecessor_observation_task_id
            || facts.predecessor_observation_result_sha256) {
            detail = "generation 1 must not claim a predecessor observation";
            return false;
        }
    } else {
        if (!facts.predecessor_observation_task_id
            || !facts.predecessor_observation_result_sha256
            || !prefixed_sha256(*facts.predecessor_observation_task_id,
                                local_continuity_observation_task_prefix)
            || !lowercase_sha256(*facts.predecessor_observation_result_sha256)) {
            detail = "later generation predecessor evidence is not canonical";
            return false;
        }
    }
    return true;
}

} // namespace

std::string local_continuity_observation_opportunity_identity(
    const LocalContinuityObservationFacts& facts)
{
    std::string detail;
    if (!validate_facts(facts, detail))
        throw std::invalid_argument(detail);

    std::string identity;
    identity.reserve(512);
    identity += "schema=";
    identity += local_continuity_observation_identity_schema;
    identity += "\nscope=";
    identity += local_continuity_observation_scope;
    identity += "\ngeneration=" + std::to_string(facts.generation);
    identity += "\ndue_at_ms=" + std::to_string(facts.due_at_ms);
    identity += "\nanchor_checkpoint_task_id=" + facts.anchor_checkpoint_task_id;
    identity += "\npredecessor_observation_task_id=";
    if (facts.predecessor_observation_task_id)
        identity += *facts.predecessor_observation_task_id;
    identity += "\npredecessor_observation_result_sha256=";
    if (facts.predecessor_observation_result_sha256)
        identity += *facts.predecessor_observation_result_sha256;
    identity += '\n';
    return identity;
}

gaudere::work::Task make_local_continuity_observation_task(
    const LocalContinuityObservationFacts& facts)
{
    const auto identity = local_continuity_observation_opportunity_identity(facts);
    const auto identity_sha = sha256_hex(identity);
    const auto payload = payload_json(facts).dump();
    if (payload.size() > max_observation_bytes)
        throw std::invalid_argument("local continuity observation exceeds size bound");

    Task task;
    task.id = std::string{local_continuity_observation_task_prefix} + identity_sha;
    task.idempotency_key = std::string{local_continuity_observation_task_prefix}
        + "opportunity:" + identity_sha;
    task.kind = local_continuity_observation_task_kind;
    task.input_content_type = local_continuity_observation_content_type;
    task.input = payload;
    task.limits.max_input_bytes = max_observation_bytes;
    task.limits.max_output_bytes = max_observation_bytes;
    task.limits.max_runtime = std::chrono::seconds{2};
    task.limits.max_attempts = 2;
    return task;
}

LocalContinuityObservationInspection inspect_local_continuity_observation_payload(
    const std::string& payload) noexcept
{
    LocalContinuityObservationInspection inspection;
    try {
        if (payload.empty() || payload.size() > max_observation_bytes) {
            inspection.detail = "payload is empty or exceeds bound";
            return inspection;
        }
        const auto parsed = Json::parse(payload);
        if (!parsed.is_object()
            || parsed.value("schema", "") != local_continuity_observation_schema
            || parsed.value("scope", "") != local_continuity_observation_scope
            || parsed.dump() != payload) {
            inspection.detail = "payload is not canonical local-observation JSON";
            return inspection;
        }

        LocalContinuityObservationFacts facts;
        std::uint64_t generation = 0;
        std::uint64_t lateness = 0;
        if (!unsigned_field(parsed, "generation", generation)
            || generation > std::numeric_limits<std::uint32_t>::max()
            || !signed_nonnegative_field(parsed, "due_at_ms", facts.due_at_ms)
            || !signed_nonnegative_field(parsed, "captured_at_ms", facts.captured_at_ms)
            || !unsigned_field(parsed, "lateness_ms", lateness)
            || !parsed.contains("anchor_checkpoint_task_id")
            || !parsed.at("anchor_checkpoint_task_id").is_string()
            || !nullable_string_field(parsed, "predecessor_observation_task_id",
                                      facts.predecessor_observation_task_id)
            || !nullable_string_field(parsed, "predecessor_observation_result_sha256",
                                      facts.predecessor_observation_result_sha256)
            || !unsigned_field(parsed, "provider_total", facts.provider_total)
            || !unsigned_field(parsed, "provider_limit", facts.provider_limit)
            || !unsigned_field(parsed, "actions_total", facts.actions_total)
            || !unsigned_field(parsed, "actions_confirmed", facts.actions_confirmed)
            || !unsigned_field(parsed, "wake_total", facts.wake_total)
            || !unsigned_field(parsed, "wake_fired", facts.wake_fired)
            || !unsigned_field(parsed, "checkpoint_count", facts.checkpoint_count)
            || !parsed.contains("latest_checkpoint_task_id")
            || !parsed.at("latest_checkpoint_task_id").is_string()) {
            inspection.detail = "payload field types are invalid";
            return inspection;
        }
        facts.generation = static_cast<std::uint32_t>(generation);
        facts.anchor_checkpoint_task_id =
            parsed.at("anchor_checkpoint_task_id").get<std::string>();
        facts.latest_checkpoint_task_id =
            parsed.at("latest_checkpoint_task_id").get<std::string>();

        if (facts.captured_at_ms < facts.due_at_ms
            || lateness != static_cast<std::uint64_t>(facts.captured_at_ms - facts.due_at_ms)) {
            inspection.detail = "lateness does not match capture minus due time";
            return inspection;
        }
        std::string detail;
        if (!validate_facts(facts, detail)) {
            inspection.detail = detail;
            return inspection;
        }
        if (payload_json(facts).dump() != payload) {
            inspection.detail = "payload contains unknown or non-canonical fields";
            return inspection;
        }

        inspection.eligible = true;
        inspection.facts = std::move(facts);
        inspection.canonical_payload = payload;
        return inspection;
    } catch (const std::exception& e) {
        inspection.detail = e.what();
        return inspection;
    } catch (...) {
        inspection.detail = "payload inspection failed";
        return inspection;
    }
}

LocalContinuityObservationInspection inspect_local_continuity_observation_task(
    const gaudere::work::Task& task) noexcept
{
    auto inspection = inspect_local_continuity_observation_payload(task.input);
    if (!inspection.eligible) return inspection;
    try {
        const auto expected = make_local_continuity_observation_task(inspection.facts);
        if (!same_definition(task, expected)) {
            inspection.eligible = false;
            inspection.detail = "Task definition does not match canonical opportunity identity";
        }
    } catch (const std::exception& e) {
        inspection.eligible = false;
        inspection.detail = e.what();
    }
    return inspection;
}

bool canonical_local_continuity_observation_success(
    const gaudere::work::Task& task) noexcept
{
    const auto inspection = inspect_local_continuity_observation_task(task);
    return inspection.eligible
        && task.status == TaskStatus::succeeded
        && task.result
        && task.result->content_type == local_continuity_observation_content_type
        && task.result->output == task.input
        && task.result->failure_code.empty()
        && task.result->failure_message.empty();
}

} // namespace gaudere_agent
