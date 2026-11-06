#include "LocalGooseDialogueResponderRetryEligibility.hpp"
#include "local_goose_dialogue_responder_retry_fixture.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>

namespace {

using namespace gaudere_agent;
using Code = LocalGooseDialogueResponderRetryEligibilityCode;

LocalGooseDialogueResponderRetryEligibilityService inspector(Fixture& fixture)
{
    return {
        fixture.completion, fixture.thread, fixture.responder,
        fixture.authority, fixture.tasks, fixture.retry
    };
}

std::string admit_for_test(Fixture& fixture)
{
    const auto write = fixture.service().prepare_lineage(fixture.request);
    assert(write.result
        == LocalGooseDialogueResponderRetryAdmissionResult::accepted);
    assert(write.lineage);
    return write.lineage->lineage_id;
}

void prove_missing_lineage_stays_missing()
{
    Fixture fixture;
    const auto state = inspector(fixture).inspect_generation_1(
        local_goose_dialogue_responder_first_retry_lineage_id, fixture.now_ms);
    assert(state.code == Code::lineage_absent);
    assert(!fixture.retry.find_for_event(
        Fixture::consumer_id, fixture.trigger_event.event_id));
    assert(fixture.completion.consumer_last_sequence(Fixture::consumer_id)
        == std::optional<std::uint64_t>{3});
}

void prove_existing_lineage_is_evidence_only()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    const auto service = inspector(fixture);

    const auto status = service.inspect_generation_1(
        lineage_id, fixture.now_ms);
    assert(status.code == Code::evidence_consistent_no_authority);
    const auto repeat = service.inspect_generation_1(
        lineage_id, fixture.now_ms + 100);
    assert(repeat.code == Code::evidence_consistent_no_authority);
    assert(fixture.retry.find_lineage(lineage_id));
    assert(fixture.completion.consumer_last_sequence(Fixture::consumer_id)
        == std::optional<std::uint64_t>{3});
    assert(fixture.completion.next_for_consumer(Fixture::consumer_id)
        ->sequence == 4);
    assert(fixture.thread.find("main")->revision == 3);
    assert(fixture.authority.find_decision(fixture.decision.decision_id)->state
        == LocalGooseDialogueResponderDecisionState::expired);
    assert(fixture.responder.find_intent(fixture.intent.intent_id)->state
        == LocalGooseDialogueResponderIntentState::expired);
    assert(fixture.responder.find_disposition(
        fixture.disposition.disposition_id)->state
        == LocalGooseDialogueResponderDispositionState::manual_review);
    assert(!fixture.tasks.submission);
}

void prove_unrelated_lineage_cannot_be_inspected_as_expected()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    assert(inspector(fixture).inspect_generation_1(
        local_goose_dialogue_responder_first_retry_lineage_id,
        fixture.now_ms).code == Code::lineage_absent);
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, -1).code == Code::unavailable);
}

void prove_stale_head_blocks_eligibility()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    const auto current = fixture.thread.find("main");
    assert(current);
    const LocalGooseDialogueThreadHead replacement{
        "main", 4, current->root_task_id, v3_id('f')};
    assert(fixture.thread.replace(*current, replacement).result
        == LocalGooseDialogueThreadStoreResult::accepted);
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, fixture.now_ms).code == Code::conflict);
}

void prove_ack_blocks_eligibility()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    assert(fixture.completion.acknowledge(Fixture::consumer_id, 4).result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, fixture.now_ms).code == Code::conflict);
}

void prove_submission_task_blocks_eligibility()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    gaudere::work::Task submitted;
    submitted.id = v3_id('f');
    submitted.idempotency_key = std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(fixture.intent.request_id);
    fixture.tasks.submission = submitted;
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, fixture.now_ms).code == Code::conflict);
}

void prove_missing_trigger_task_blocks_eligibility()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    fixture.tasks.trigger.reset();
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, fixture.now_ms).code == Code::conflict);
}

void prove_expiry_window_blocks_eligibility()
{
    Fixture fixture;
    const auto lineage_id = admit_for_test(fixture);
    assert(inspector(fixture).inspect_generation_1(
        lineage_id, 2000).code == Code::conflict);
}

} // namespace

int main()
{
    prove_missing_lineage_stays_missing();
    prove_existing_lineage_is_evidence_only();
    prove_unrelated_lineage_cannot_be_inspected_as_expected();
    prove_stale_head_blocks_eligibility();
    prove_ack_blocks_eligibility();
    prove_submission_task_blocks_eligibility();
    prove_missing_trigger_task_blocks_eligibility();
    prove_expiry_window_blocks_eligibility();
    return 0;
}
