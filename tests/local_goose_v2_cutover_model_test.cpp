#include "LocalGooseV2CutoverModel.hpp"

#include "LocalContinuityObservation.hpp"
#include "LocalGooseCycle.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace {
using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;
using Json = nlohmann::json;
using Phase = LocalGooseV2CutoverPhase;
using Event = LocalGooseV2CutoverEvent;
using Result = LocalGooseV2CutoverResult;
using Record = LocalGooseV2CutoverRecord;

std::string hex(char c) { return std::string(64, c); }

Task observation(std::uint32_t generation,
                 const std::optional<Task>& predecessor = std::nullopt)
{
    LocalContinuityObservationFacts facts;
    facts.generation = generation;
    facts.due_at_ms = 1000 * static_cast<std::int64_t>(generation);
    facts.captured_at_ms = facts.due_at_ms + 10;
    facts.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + hex('a');
    facts.anchor_checkpoint_result_sha256 = hex('a');
    if (predecessor) {
        facts.predecessor_observation_task_id = predecessor->id;
        facts.predecessor_observation_result_sha256 =
            sha256_hex(predecessor->result->output);
    }
    facts.provider_scope = "provider.call:openai.responses";
    facts.provider_total = 10;
    facts.provider_limit = 12;
    facts.predecessor_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('b');
    facts.audited_provider_action_id =
        "provider.call:openai.responses:cognition.current.v0:" + hex('c');
    facts.historical_wake_scope = "cognition.reflect.wake.v0";
    facts.historical_wake_sha256 = hex('d');
    auto task = make_local_continuity_observation_task(facts);
    task.status = TaskStatus::succeeded;
    task.result = TaskResult{local_continuity_observation_content_type,
                             task.input, {}, {}};
    assert(canonical_local_continuity_observation_success(task));
    return task;
}

Task anchor()
{
    const auto one = observation(1);
    const auto two = observation(2, one);
    return observation(3, two);
}

std::string idle_decision()
{
    return Json{
        {"assessment", "SENSITIVE-LOCAL-HISTORY"},
        {"decision", "idle"},
        {"next_wake_after_ms", nullptr},
        {"openai_request", nullptr},
        {"reason", "SENSITIVE-REASON"},
        {"schema", local_goose_decision_schema}
    }.dump();
}

struct Fixture {
    Task fixed_anchor = anchor();
    std::string model = hex('e');
    Task first;
    LocalGooseCycleCursor cursor;
    std::map<std::string, Task> store;

    Fixture()
    {
        first = make_local_goose_cycle_task(
            fixed_anchor, model, 1, 10000, 10001);
        first.status = TaskStatus::succeeded;
        first.result = TaskResult{
            local_goose_decision_content_type, idle_decision(), {}, {}};
        assert(canonical_local_goose_cycle_success(first));
        cursor.revision = 7;
        cursor.generation = 2;
        cursor.anchor_observation_task_id = fixed_anchor.id;
        cursor.anchor_observation_result_sha256 =
            sha256_hex(fixed_anchor.result->output);
        cursor.predecessor_task_id = first.id;
        cursor.predecessor_result_sha256 = sha256_hex(first.result->output);
        store.emplace(fixed_anchor.id, fixed_anchor);
        store.emplace(first.id, first);
        assert(valid_local_goose_cycle_cursor(cursor));
    }

    std::optional<Task> read(const std::string& id) const
    {
        const auto it = store.find(id);
        return it == store.end() ? std::nullopt
            : std::optional<Task>{it->second};
    }

    LocalGooseV2BridgeInspection bridge() const
    {
        return inspect_local_goose_v2_dormant_bridge(
            [this] { return std::optional<LocalGooseCycleCursor>{cursor}; },
            [this](const std::string& id) { return read(id); }, model);
    }

    Task preview(std::int64_t due = 20000,
                 std::int64_t captured = 20001) const
    {
        return make_local_goose_cycle_v2_task(
            fixed_anchor, model, 2, due, captured, first,
            [this](const std::string& id) { return read(id); });
    }

    LocalGooseV2CutoverCandidate candidate() const
    {
        return make_local_goose_v2_cutover_candidate(
            cursor, bridge(), preview(), model);
    }
};

Record accepted(const LocalGooseV2CutoverStep& step, Phase phase)
{
    assert(step.result == Result::accepted);
    assert(step.record && step.record->phase == phase);
    assert(valid_local_goose_v2_cutover_record(*step.record));
    return *step.record;
}

void denied(const LocalGooseV2CutoverStep& step, Result status)
{
    assert(step.result == status && !step.record);
}
} // namespace

int main()
{
    Fixture f;
    const auto proof = f.bridge();
    assert(proof.status
           == LocalGooseV2BridgeStatus::verified_dormant_no_authority);
    const auto candidate = f.candidate();
    assert(candidate.eligible && candidate.detail.empty());
    assert(candidate.canonical_json.size() < 4096);
    assert(candidate.canonical_json.find("SENSITIVE-") == std::string::npos);
    const auto next = f.candidate();
    assert(next.eligible);
    assert(next.candidate_id == candidate.candidate_id);
    assert(next.v2_task_id == candidate.v2_task_id);
    const auto frozen = Json::parse(candidate.canonical_json);
    assert(frozen.at("cursor_revision") == 7);
    assert(frozen.at("generation") == 2);
    assert(frozen.at("v2_input_sha256") == candidate.v2_input_sha256);
    assert(frozen.at("bounded_memory_sha256")
           == proof.evidence->bounded_memory_sha256);

    // A future record starts as a pure proposal, not as authority or a wake.
    auto record = initial_local_goose_v2_cutover_record(candidate);
    assert(record && record->phase == Phase::proposal);
    assert(record->revision == 0 && record->fence_epoch == 0);
    denied(simulate_local_goose_v2_cutover(
        *record, Event::freeze_identity, 0, 0), Result::invalid);
    denied(simulate_local_goose_v2_cutover(
        *record, Event::fence_v1, 0, 0), Result::invalid);
    denied(simulate_local_goose_v2_cutover(
        *record, Event::fence_v1, 0,
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max()) + 1), Result::invalid);
    auto fenced = accepted(simulate_local_goose_v2_cutover(
        *record, Event::fence_v1, 0, 42), Phase::fenced);
    assert(fenced.fence_epoch == 42);
    const auto duplicate_fence = simulate_local_goose_v2_cutover(
        fenced, Event::fence_v1, 0, 42);
    assert(duplicate_fence.result == Result::duplicate
           && duplicate_fence.record
           && duplicate_fence.record->revision == 1);
    denied(simulate_local_goose_v2_cutover(
        fenced, Event::fence_v1, 0, 43), Result::conflict);
    denied(simulate_local_goose_v2_cutover(
        fenced, Event::freeze_identity, 1, 43), Result::conflict);
    denied(simulate_local_goose_v2_cutover(
        fenced, Event::confirm_publish, 1, 42,
        candidate.v2_task_id, candidate.v2_input_sha256), Result::invalid);

    const auto frozen_record = accepted(simulate_local_goose_v2_cutover(
        fenced, Event::freeze_identity, 1, 42), Phase::frozen);
    assert(frozen_record.v2_task_id == candidate.v2_task_id);
    denied(simulate_local_goose_v2_cutover(
        frozen_record, Event::abort_pre_freeze, 2, 42), Result::invalid);
    denied(simulate_local_goose_v2_cutover(
        frozen_record, Event::confirm_publish, 2, 42,
        candidate.v2_task_id, hex('f')), Result::invalid);
    denied(simulate_local_goose_v2_cutover(
        frozen_record, Event::confirm_publish, 2, 42,
        std::string(local_goose_cycle_v2_task_prefix) + hex('f'),
        candidate.v2_input_sha256), Result::invalid);

    const auto published = accepted(simulate_local_goose_v2_cutover(
        frozen_record, Event::confirm_publish, 2, 42,
        candidate.v2_task_id, candidate.v2_input_sha256), Phase::published);
    assert(published.revision == 3);
    const auto duplicate_publish = simulate_local_goose_v2_cutover(
        published, Event::confirm_publish, 2, 42,
        candidate.v2_task_id, candidate.v2_input_sha256);
    assert(duplicate_publish.result == Result::duplicate
        && duplicate_publish.record);
    denied(simulate_local_goose_v2_cutover(
        published, Event::abort_pre_freeze, 3, 42), Result::invalid);
    const auto quarantined = accepted(simulate_local_goose_v2_cutover(
        published, Event::quarantine, 3, 42), Phase::quarantined);
    denied(simulate_local_goose_v2_cutover(
        quarantined, Event::abort_pre_freeze, 4, 42), Result::invalid);
    denied(simulate_local_goose_v2_cutover(
        quarantined, Event::fence_v1, 4, 42), Result::invalid);

    // Reversible by protocol only while no frozen Task identity was committed.
    const auto pre_fence_abort = accepted(simulate_local_goose_v2_cutover(
        *record, Event::abort_pre_freeze, 0, 0), Phase::aborted);
    assert(pre_fence_abort.fence_epoch == 0);
    denied(simulate_local_goose_v2_cutover(
        pre_fence_abort, Event::fence_v1, 1, 100), Result::invalid);
    const auto fenced_abort = accepted(simulate_local_goose_v2_cutover(
        fenced, Event::abort_pre_freeze, 1, 42), Phase::aborted);
    assert(fenced_abort.revision == 2 && fenced_abort.fence_epoch == 42);
    denied(simulate_local_goose_v2_cutover(
        fenced_abort, Event::freeze_identity, 2, 42), Result::invalid);
    const auto frozen_quarantine = accepted(simulate_local_goose_v2_cutover(
        frozen_record, Event::quarantine, 2, 42), Phase::quarantined);
    assert(frozen_quarantine.revision == 3);
    denied(simulate_local_goose_v2_cutover(
        frozen_quarantine, Event::confirm_publish, 3, 42,
        candidate.v2_task_id, candidate.v2_input_sha256), Result::invalid);

    // Loss of durable source evidence or a forged memory digest blocks preview.
    { auto stale = f.cursor; ++stale.revision;
      assert(!make_local_goose_v2_cutover_candidate(
          stale, proof, f.preview(), f.model).eligible); }
    { auto stale = f.cursor; stale.state = LocalGooseCycleState::scheduled;
      stale.due_at_ms = 20000;
      assert(!make_local_goose_v2_cutover_candidate(
          stale, proof, f.preview(), f.model).eligible); }
    { auto altered = proof; altered.evidence->bounded_memory_sha256 = hex('f');
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, altered, f.preview(), f.model).eligible); }
    { auto altered = proof; altered.evidence->bounded_memory_entries = 9;
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, altered, f.preview(), f.model).eligible); }
    { auto altered = proof; altered.evidence->next_generation = 3;
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, altered, f.preview(), f.model).eligible); }
    { auto altered = proof; altered.status =
          LocalGooseV2BridgeStatus::snapshot_changed;
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, altered, f.preview(), f.model).eligible); }
    { assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, proof, f.preview(), hex('f')).eligible); }
    { auto executed = f.preview();
      executed.status = TaskStatus::succeeded;
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, proof, executed, f.model).eligible); }
    { auto leased = f.preview();
      leased.attempts_started = 1;
      assert(!make_local_goose_v2_cutover_candidate(
          f.cursor, proof, leased, f.model).eligible); }
    { auto different = f.preview(20002, 20003);
      const auto new_candidate = make_local_goose_v2_cutover_candidate(
          f.cursor, proof, different, f.model);
      assert(new_candidate.eligible
          && new_candidate.candidate_id != candidate.candidate_id);
      // A different timestamp can only be a different *unapproved* plan.
    }
    { auto fake = candidate;
      fake.canonical_json += " ";
      assert(!initial_local_goose_v2_cutover_record(fake)); }
    { auto fake = candidate;
      fake.v2_task_id += "0";
      assert(!initial_local_goose_v2_cutover_record(fake)); }
    { auto fake = candidate;
      auto json = Json::parse(fake.canonical_json);
      json["bounded_memory_sha256"] = "PRIVATE-FAKE-MEMORY";
      fake.canonical_json = json.dump();
      fake.candidate_id =
          std::string(local_goose_v2_cutover_candidate_prefix)
          + sha256_hex(fake.canonical_json);
      assert(!initial_local_goose_v2_cutover_record(fake)); }
    { auto fake = candidate;
      auto json = Json::parse(fake.canonical_json);
      json["due_at_ms"] = static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()) + 1;
      fake.canonical_json = json.dump();
      fake.candidate_id =
          std::string(local_goose_v2_cutover_candidate_prefix)
          + sha256_hex(fake.canonical_json);
      assert(!initial_local_goose_v2_cutover_record(fake)); }
    { Record fake = *record; fake.revision = 99;
      assert(!valid_local_goose_v2_cutover_record(fake));
      denied(simulate_local_goose_v2_cutover(
          fake, Event::fence_v1, 99, 42), Result::invalid); }
    assert(f.cursor.state == LocalGooseCycleState::dormant);
    assert(!f.cursor.due_at_ms);
    std::cout << "Local Goose V1 to V2 cutover source-only simulation: ok\n";
}
