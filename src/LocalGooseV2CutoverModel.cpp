#include "LocalGooseV2CutoverModel.hpp"

#include "Sha256.hpp"
#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"

#include <nlohmann/json.hpp>

#include <limits>
#include <set>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {
using Json = nlohmann::json;
using Phase = LocalGooseV2CutoverPhase;
using Event = LocalGooseV2CutoverEvent;
using Result = LocalGooseV2CutoverResult;
using Record = LocalGooseV2CutoverRecord;
using Step = LocalGooseV2CutoverStep;

constexpr std::size_t max_candidate_bytes = 4096;

bool digest(const std::string& s) noexcept
{
    if (s.size() != 64) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

bool identity(const std::string& value, const char* prefix) noexcept
{
    const std::string p{prefix};
    return value.size() == p.size() + 64
        && value.compare(0, p.size(), p) == 0
        && digest(value.substr(p.size()));
}

bool candidate_consistent(const LocalGooseV2CutoverCandidate& candidate)
{
    if (!candidate.eligible || candidate.canonical_json.empty()
        || candidate.canonical_json.size() > max_candidate_bytes
        || !identity(candidate.candidate_id,
                     local_goose_v2_cutover_candidate_prefix)
        || !identity(candidate.v2_task_id,
                     local_goose_cycle_v2_task_prefix)
        || !digest(candidate.v2_input_sha256)
        || candidate.candidate_id !=
           std::string(local_goose_v2_cutover_candidate_prefix)
               + sha256_hex(candidate.canonical_json))
        return false;

    const auto doc = Json::parse(candidate.canonical_json);
    if (!doc.is_object() || doc.size() != 13
        || doc.dump() != candidate.canonical_json
        || !doc.contains("schema")
        || doc.at("schema") != local_goose_v2_cutover_candidate_schema
        || !doc.contains("v2_task_id")
        || doc.at("v2_task_id") != candidate.v2_task_id
        || !doc.contains("v2_input_sha256")
        || doc.at("v2_input_sha256") != candidate.v2_input_sha256)
        return false;
    static const std::set<std::string> keys{
        "anchor_observation_result_sha256", "anchor_observation_task_id",
        "bounded_memory_sha256", "captured_at_ms", "cursor_revision",
        "due_at_ms", "generation", "model_sha256", "predecessor_result_sha256",
        "predecessor_task_id", "schema", "v2_input_sha256", "v2_task_id"
    };
    for (const auto& kv : doc.items())
        if (keys.count(kv.key()) != 1) return false;

    const auto& revision = doc.at("cursor_revision");
    const auto& generation = doc.at("generation");
    const auto& due = doc.at("due_at_ms");
    const auto& captured = doc.at("captured_at_ms");
    const auto nonnegative_i64 = [](const Json& value) {
        if (!value.is_number_integer()) return false;
        if (value.is_number_unsigned())
            return value.get<std::uint64_t>() <=
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max());
        return value.get<std::int64_t>() >= 0;
    };
    if (!revision.is_number_unsigned() || !generation.is_number_unsigned()
        || revision.get<std::uint64_t>() >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || generation.get<std::uint64_t>() < 2
        || generation.get<std::uint64_t>() >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())
        || !nonnegative_i64(due) || !nonnegative_i64(captured)
        || due.get<std::uint64_t>() > captured.get<std::uint64_t>())
        return false;
    if (!doc.at("anchor_observation_task_id").is_string()
        || !identity(doc.at("anchor_observation_task_id").get<std::string>(),
                     local_continuity_observation_task_prefix)
        || !doc.at("anchor_observation_result_sha256").is_string()
        || !digest(doc.at("anchor_observation_result_sha256").get<std::string>())
        || !doc.at("predecessor_task_id").is_string()
        || !identity(doc.at("predecessor_task_id").get<std::string>(),
                     local_goose_cycle_task_prefix)
        || !doc.at("predecessor_result_sha256").is_string()
        || !digest(doc.at("predecessor_result_sha256").get<std::string>())
        || !doc.at("model_sha256").is_string()
        || !digest(doc.at("model_sha256").get<std::string>())
        || !doc.at("bounded_memory_sha256").is_string()
        || !digest(doc.at("bounded_memory_sha256").get<std::string>()))
        return false;
    return true;
}

bool same_receipt(const Record& record,
                  const std::string_view id,
                  const std::string_view input_sha) noexcept
{
    return id == record.v2_task_id && input_sha == record.v2_input_sha256;
}

bool duplicate(const Record& current, const Event event,
               const std::uint64_t expected_revision,
               const std::uint64_t epoch,
               const std::string_view receipt_id,
               const std::string_view receipt_sha) noexcept
{
    if (current.revision == 0 || expected_revision != current.revision - 1)
        return false;
    switch (event) {
    case Event::fence_v1:
        return current.phase == Phase::fenced && epoch == current.fence_epoch;
    case Event::freeze_identity:
        return current.phase == Phase::frozen && epoch == current.fence_epoch;
    case Event::confirm_publish:
        return current.phase == Phase::published && epoch == current.fence_epoch
            && same_receipt(current, receipt_id, receipt_sha);
    case Event::abort_pre_freeze:
        return current.phase == Phase::aborted && epoch == current.fence_epoch;
    case Event::quarantine:
        return current.phase == Phase::quarantined && epoch == current.fence_epoch;
    }
    return false;
}

Step fail(const Result result) { return {result, std::nullopt}; }

} // namespace

LocalGooseV2CutoverCandidate make_local_goose_v2_cutover_candidate(
    const LocalGooseCycleCursor& cursor,
    const LocalGooseV2BridgeInspection& bridge,
    const gaudere::work::Task& v2,
    const std::string& model) noexcept
{
    LocalGooseV2CutoverCandidate out;
    try {
        if (!valid_local_goose_cycle_cursor(cursor)
            || cursor.state != LocalGooseCycleState::dormant
            || cursor.generation < 2
            || !digest(model)
            || bridge.status !=
               LocalGooseV2BridgeStatus::verified_dormant_no_authority
            || !bridge.evidence
            || !cursor.predecessor_task_id
            || !cursor.predecessor_result_sha256) {
            out.detail = "V1 dormant or bridge evidence invalid";
            return out;
        }
        // A candidate can describe only a hypothetical not-yet-submitted
        // Task. A persisted or active V2 Task is not a fresh preview.
        if (v2.status != gaudere::work::TaskStatus::pending
            || v2.attempts_started != 0 || v2.lease
            || v2.result || !v2.cancel_reason.empty()) {
            out.detail = "V2 candidate Task has mutable execution state";
            return out;
        }
        const auto& evidence = *bridge.evidence;
        const auto inspected = inspect_local_goose_cycle_v2_task(v2);
        if (!inspected.eligible
            || evidence.cursor_revision != cursor.revision
            || evidence.next_generation != cursor.generation
            || evidence.predecessor_task_id != *cursor.predecessor_task_id
            || evidence.predecessor_result_sha256
                != *cursor.predecessor_result_sha256
            || !digest(evidence.bounded_memory_sha256)
            || inspected.generation != cursor.generation
            || inspected.predecessor_task_id != *cursor.predecessor_task_id
            || sha256_hex(inspected.memory_json)
                != evidence.bounded_memory_sha256) {
            out.detail = "frozen V2 preview does not match V1 observation";
            return out;
        }

        const Json input = Json::parse(inspected.canonical_input);
        const auto& frozen = input.at("memory");
        if (input.at("anchor_observation_task_id")
                != cursor.anchor_observation_task_id
            || input.at("anchor_observation_result_sha256")
                != cursor.anchor_observation_result_sha256
            || input.at("predecessor_result_sha256")
                != *cursor.predecessor_result_sha256
            || input.at("model_sha256") != model
            || frozen.at("entries").size() != evidence.bounded_memory_entries
            || frozen.at("earlier_omitted")
                != evidence.earlier_generations_omitted) {
            out.detail = "frozen Task anchor/model/memory mismatch";
            return out;
        }
        Json candidate{
            {"anchor_observation_result_sha256",
             cursor.anchor_observation_result_sha256},
            {"anchor_observation_task_id", cursor.anchor_observation_task_id},
            {"bounded_memory_sha256", evidence.bounded_memory_sha256},
            {"captured_at_ms", input.at("captured_at_ms")},
            {"cursor_revision", cursor.revision},
            {"due_at_ms", input.at("due_at_ms")},
            {"generation", cursor.generation},
            {"model_sha256", model},
            {"predecessor_result_sha256", *cursor.predecessor_result_sha256},
            {"predecessor_task_id", *cursor.predecessor_task_id},
            {"schema", local_goose_v2_cutover_candidate_schema},
            {"v2_input_sha256", sha256_hex(inspected.canonical_input)},
            {"v2_task_id", v2.id}
        };
        out.canonical_json = candidate.dump();
        if (out.canonical_json.size() > max_candidate_bytes) {
            out.canonical_json.clear();
            out.detail = "cutover preview exceeds fixed bound";
            return out;
        }
        out.candidate_id = std::string(local_goose_v2_cutover_candidate_prefix)
            + sha256_hex(out.canonical_json);
        out.v2_task_id = v2.id;
        out.v2_input_sha256 = sha256_hex(inspected.canonical_input);
        // Self-check expects an explicitly eligible candidate. The producer
        // sets it before checking; a failed check must clear it again.
        out.eligible = true;
        if (!candidate_consistent(out)) {
            out.eligible = false;
            out.detail = "cutover candidate self-check failed";
        }
    } catch (...) {
        out = {};
        out.detail = "cutover candidate failed closed";
    }
    return out;
}

bool valid_local_goose_v2_cutover_record(const Record& r) noexcept
{
    if (!identity(r.candidate_id, local_goose_v2_cutover_candidate_prefix)
        || !identity(r.v2_task_id, local_goose_cycle_v2_task_prefix)
        || !digest(r.v2_input_sha256)
        || r.fence_epoch >
           static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return false;

    switch (r.phase) {
    case Phase::proposal:
        return r.revision == 0 && r.fence_epoch == 0;
    case Phase::fenced:
        return r.revision == 1 && r.fence_epoch != 0;
    case Phase::frozen:
        return r.revision == 2 && r.fence_epoch != 0;
    case Phase::published:
        return r.revision == 3 && r.fence_epoch != 0;
    case Phase::aborted:
        return (r.revision == 1 && r.fence_epoch == 0)
            || (r.revision == 2 && r.fence_epoch != 0);
    case Phase::quarantined:
        return r.fence_epoch != 0
            && (r.revision == 2 || r.revision == 3 || r.revision == 4);
    }
    return false;
}

std::optional<Record> initial_local_goose_v2_cutover_record(
    const LocalGooseV2CutoverCandidate& candidate) noexcept
{
    try {
        if (!candidate_consistent(candidate)) return std::nullopt;
        Record initial;
        initial.candidate_id = candidate.candidate_id;
        initial.v2_task_id = candidate.v2_task_id;
        initial.v2_input_sha256 = candidate.v2_input_sha256;
        return valid_local_goose_v2_cutover_record(initial)
            ? std::optional<Record>{initial} : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

LocalGooseV2CutoverStep simulate_local_goose_v2_cutover(
    const Record& current, const Event event,
    const std::uint64_t expected_revision, const std::uint64_t epoch,
    const std::string_view receipt_id,
    const std::string_view receipt_sha) noexcept
{
    if (!valid_local_goose_v2_cutover_record(current))
        return fail(Result::invalid);
    if (event != Event::confirm_publish
        && (!receipt_id.empty() || !receipt_sha.empty()))
        return fail(Result::invalid);
    if (current.revision != expected_revision) {
        if (duplicate(current, event, expected_revision, epoch,
                      receipt_id, receipt_sha))
            return {Result::duplicate, current};
        return fail(Result::conflict);
    }
    // A terminal ledger cannot be restarted, regardless of a forged or
    // stale owner epoch. Preserve only exact previous-transition replay
    // handled above as a duplicate.
    if (current.phase == Phase::aborted
        || current.phase == Phase::quarantined)
        return fail(Result::invalid);
    if (current.phase != Phase::proposal
        && epoch != current.fence_epoch)
        return fail(Result::conflict);

    Record next = current;
    switch (current.phase) {
    case Phase::proposal:
        if (event == Event::fence_v1
            && epoch > 0
            && epoch <= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            next.phase = Phase::fenced;
            next.fence_epoch = epoch;
        } else if (event == Event::abort_pre_freeze && epoch == 0) {
            next.phase = Phase::aborted;
        } else {
            return fail(Result::invalid);
        }
        break;
    case Phase::fenced:
        if (event == Event::freeze_identity)
            next.phase = Phase::frozen;
        else if (event == Event::abort_pre_freeze)
            next.phase = Phase::aborted;
        else if (event == Event::quarantine)
            next.phase = Phase::quarantined;
        else
            return fail(Result::invalid);
        break;
    case Phase::frozen:
        if (event == Event::confirm_publish
            && same_receipt(current, receipt_id, receipt_sha))
            next.phase = Phase::published;
        else if (event == Event::quarantine)
            next.phase = Phase::quarantined;
        else
            return fail(Result::invalid);
        break;
    case Phase::published:
        if (event == Event::quarantine)
            next.phase = Phase::quarantined;
        else
            return fail(Result::invalid);
        break;
    case Phase::aborted:
    case Phase::quarantined:
        return fail(Result::invalid);
    }
    ++next.revision;
    if (!valid_local_goose_v2_cutover_record(next))
        return fail(Result::invalid);
    return {Result::accepted, next};
}
} // namespace gaudere_agent
