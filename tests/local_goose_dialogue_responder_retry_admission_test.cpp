#include "local_goose_dialogue_responder_retry_fixture.hpp"

namespace {

void prove_success_and_idempotent_replay()
{
    Fixture fixture;
    auto service = fixture.service();
    const auto accepted = service.prepare_lineage(fixture.request);
    assert(accepted.result
        == LocalGooseDialogueResponderRetryAdmissionResult::accepted);
    assert(accepted.lineage);
    assert(accepted.lineage->retry_generation == 1);
    assert(accepted.lineage->completion_sequence == 4);
    assert(accepted.lineage->event_id == fixture.trigger_event.event_id);
    assert(accepted.lineage->historical_grant_terminal_reason.empty());
    assert(accepted.lineage->historical_lease_terminal_reason == "lease_expired");
    assert(accepted.lineage->historical_decision_terminal_reason
        == "bound_intent_expired_before_dispatch");
    assert(accepted.lineage->historical_intent_terminal_reason
        == "lease_or_intent_expired");
    assert(accepted.lineage->historical_disposition_terminal_reason
        == "lease_or_intent_expired");
    const auto lineage_id = accepted.lineage->lineage_id;
    assert(accepted.lineage->created_at_ms == 30000);

    fixture.now_ms = 31000;
    const auto replay = service.prepare_lineage(fixture.request);
    assert(replay.result
        == LocalGooseDialogueResponderRetryAdmissionResult::duplicate);
    assert(replay.lineage);
    assert(replay.lineage->lineage_id == lineage_id);
    assert(replay.lineage->created_at_ms == 30000);

    assert(fixture.completion.consumer_last_sequence(Fixture::consumer_id)
        == std::optional<std::uint64_t>{3});
    const auto next = fixture.completion.next_for_consumer(Fixture::consumer_id);
    assert(next && next->sequence == 4);
    const auto head = fixture.thread.find("main");
    assert(head && head->revision == 3
        && head->head_task_id == fixture.trigger_event.task_id);
    assert(fixture.responder.find_lease(Fixture::historical_lease_id)->turns_committed
        == 0);
    assert(fixture.authority.find_decision(fixture.decision.decision_id)->state
        == LocalGooseDialogueResponderDecisionState::expired);
    assert(fixture.responder.find_intent(fixture.intent.intent_id)->state
        == LocalGooseDialogueResponderIntentState::expired);
    assert(fixture.responder.find_disposition(fixture.disposition.disposition_id)->state
        == LocalGooseDialogueResponderDispositionState::manual_review);
}

void prove_submission_task_blocks_admission()
{
    Fixture fixture;
    gaudere::work::Task submitted;
    submitted.id = v3_id('f');
    submitted.idempotency_key = std::string{local_goose_dialogue_v3_task_prefix}
        + "request-id:" + sha256_hex(fixture.intent.request_id);
    fixture.tasks.submission = submitted;
    auto service = fixture.service();
    const auto result = service.prepare_lineage(fixture.request);
    assert(result.result
        == LocalGooseDialogueResponderRetryAdmissionResult::conflict);
    assert(!fixture.retry.find_for_event(
        Fixture::consumer_id, fixture.trigger_event.event_id));
}

void prove_stale_head_blocks_admission()
{
    Fixture fixture;
    const auto current = fixture.thread.find("main");
    assert(current);
    const LocalGooseDialogueThreadHead replacement{
        "main", 4, current->root_task_id, v3_id('f')};
    assert(fixture.thread.replace(*current, replacement).result
        == LocalGooseDialogueThreadStoreResult::accepted);
    auto service = fixture.service();
    const auto result = service.prepare_lineage(fixture.request);
    assert(result.result
        == LocalGooseDialogueResponderRetryAdmissionResult::conflict);
    assert(!fixture.retry.find_for_event(
        Fixture::consumer_id, fixture.trigger_event.event_id));
}

void prove_consumed_trigger_blocks_admission()
{
    Fixture fixture;
    assert(fixture.completion.acknowledge(Fixture::consumer_id, 4).result
        == LocalGooseDialogueCompletionFeedResult::accepted);
    auto service = fixture.service();
    const auto result = service.prepare_lineage(fixture.request);
    assert(result.result
        == LocalGooseDialogueResponderRetryAdmissionResult::conflict);
    assert(!fixture.retry.find_for_event(
        Fixture::consumer_id, fixture.trigger_event.event_id));
}

} // namespace

int main()
{
    prove_success_and_idempotent_replay();
    prove_submission_task_blocks_admission();
    prove_stale_head_blocks_admission();
    prove_consumed_trigger_blocks_admission();
    return 0;
}
