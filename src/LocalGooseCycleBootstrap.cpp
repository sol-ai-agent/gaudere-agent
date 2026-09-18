#include "LocalGooseCycleBootstrap.hpp"

#include "LocalContinuityObservation.hpp"
#include "Sha256.hpp"

namespace gaudere_agent {
namespace {

LocalGooseCycleBootstrapStep mapped_write(
    const LocalGooseCycleStoreWrite& write,
    const LocalGooseCycleBootstrapResult accepted,
    const LocalGooseCycleBootstrapResult duplicate)
{
    switch (write.result) {
    case LocalGooseCycleStoreResult::accepted:
        return {accepted, write.cursor, write.detail};
    case LocalGooseCycleStoreResult::duplicate:
        return {duplicate, write.cursor, write.detail};
    case LocalGooseCycleStoreResult::conflict:
        return {LocalGooseCycleBootstrapResult::conflict,
                write.cursor, write.detail};
    case LocalGooseCycleStoreResult::invalid:
        return {LocalGooseCycleBootstrapResult::invalid,
                write.cursor, write.detail};
    case LocalGooseCycleStoreResult::unavailable:
        return {LocalGooseCycleBootstrapResult::unavailable,
                write.cursor, write.detail};
    }
    return {LocalGooseCycleBootstrapResult::invalid, {}, "unknown store result"};
}

} // namespace

LocalGooseCycleBootstrapStep seed_local_goose_cycle(
    LocalGooseCycleStore& cycle_store,
    gaudere::work::TaskStore& task_store,
    const LocalActivityPulseCursor& activity_cursor)
{
    if (!valid_local_activity_pulse_cursor(activity_cursor)
        || activity_cursor.generation != 3
        || activity_cursor.state != LocalActivityPulseState::quiescent
        || activity_cursor.task_id.empty()
        || !activity_cursor.result_sha256) {
        return {LocalGooseCycleBootstrapResult::invalid, {},
                "local activity cursor is not canonical generation-3 quiescent"};
    }

    const auto anchor = task_store.find(activity_cursor.task_id);
    if (!anchor || !anchor->result
        || !canonical_local_continuity_observation_success(*anchor)
        || sha256_hex(anchor->result->output) != *activity_cursor.result_sha256) {
        return {LocalGooseCycleBootstrapResult::invalid, {},
                "final local observation Task/result is missing or non-canonical"};
    }

    LocalGooseCycleCursor seed;
    seed.anchor_observation_task_id = anchor->id;
    seed.anchor_observation_result_sha256 =
        sha256_hex(anchor->result->output);

    return mapped_write(
        cycle_store.seed(seed),
        LocalGooseCycleBootstrapResult::seeded,
        LocalGooseCycleBootstrapResult::duplicate);
}

LocalGooseCycleBootstrapStep activate_local_goose_cycle(
    LocalGooseCycleStore& cycle_store,
    const std::int64_t due_at_ms)
{
    if (due_at_ms < 0) {
        return {LocalGooseCycleBootstrapResult::invalid, {},
                "activation deadline is negative"};
    }

    const auto found = cycle_store.find(local_goose_cycle_scope);
    if (!found) {
        return {LocalGooseCycleBootstrapResult::unavailable, {},
                "Local Goose cycle sidecar is unseeded"};
    }

    if (found->state == LocalGooseCycleState::scheduled
        && found->generation == 1
        && found->revision == 1
        && !found->predecessor_task_id
        && !found->predecessor_result_sha256) {
        return {LocalGooseCycleBootstrapResult::already_active, found,
                "first Local Goose cycle is already scheduled"};
    }

    if (found->state != LocalGooseCycleState::dormant
        || found->generation != 0
        || found->revision != 0
        || found->predecessor_task_id
        || found->predecessor_result_sha256) {
        return {LocalGooseCycleBootstrapResult::conflict, found,
                "bootstrap activation requires the exact dormant generation-0 seed"};
    }

    LocalGooseCycleCursor scheduled = *found;
    scheduled.revision = 1;
    scheduled.generation = 1;
    scheduled.state = LocalGooseCycleState::scheduled;
    scheduled.due_at_ms = due_at_ms;

    if (!valid_local_goose_cycle_transition(*found, scheduled)) {
        return {LocalGooseCycleBootstrapResult::invalid, found,
                "bootstrap scheduled transition is non-canonical"};
    }

    return mapped_write(
        cycle_store.replace(*found, scheduled),
        LocalGooseCycleBootstrapResult::activated,
        LocalGooseCycleBootstrapResult::already_active);
}

} // namespace gaudere_agent
