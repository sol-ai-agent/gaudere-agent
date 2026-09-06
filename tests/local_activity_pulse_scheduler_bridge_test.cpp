#include "LocalActivityPulseSchedulerBridge.hpp"
#include "LocalActivityPulseStore.hpp"

#include <gaudere/scheduling/wake/Scheduler.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>

namespace {

using namespace gaudere_agent;
using Scheduler = gaudere::scheduling::wake::Scheduler;

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

LocalActivityPulseCursor idle_cursor(const std::int64_t anchor,
                                     const std::int64_t due)
{
    LocalActivityPulseCursor cursor;
    cursor.anchor_checkpoint_task_id =
        "continuity.delta-checkpoint.v1:" + repeated('a');
    cursor.anchor_checkpoint_result_sha256 = repeated('b');
    cursor.anchor_at_ms = anchor;
    cursor.due_at_ms = due;
    return cursor;
}

std::int64_t milliseconds(const Scheduler::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

} // namespace

int main()
{
    const auto no_cursor_disabled =
        inspect_local_activity_pulse_deadline(std::nullopt, false);
    expect(no_cursor_disabled.eligible && !no_cursor_disabled.active
               && !no_cursor_disabled.deadline,
           "disabled pulse is eligible but has no deadline");

    const auto no_cursor_enabled =
        inspect_local_activity_pulse_deadline(std::nullopt, true);
    expect(no_cursor_enabled.eligible && !no_cursor_enabled.active
               && !no_cursor_enabled.deadline,
           "enabled but unseeded pulse has no deadline");

    auto idle = idle_cursor(1'000, 2'000);
    expect(valid_local_activity_pulse_cursor(idle),
           "idle deadline fixture is canonical");
    const auto idle_deadline = inspect_local_activity_pulse_deadline(idle, true);
    expect(idle_deadline.eligible && idle_deadline.active
               && idle_deadline.deadline
               && milliseconds(*idle_deadline.deadline) == 2'000,
           "idle cursor exposes its exact durable due time");

    auto preparing = idle;
    preparing.revision = 1;
    preparing.generation = 1;
    preparing.state = LocalActivityPulseState::preparing;
    preparing.captured_at_ms = 2'500;
    preparing.task_id = "continuity.local-observation.v1:" + repeated('c');
    expect(valid_local_activity_pulse_cursor(preparing),
           "preparing deadline fixture is canonical");
    const auto preparing_deadline =
        inspect_local_activity_pulse_deadline(preparing, true);
    expect(preparing_deadline.eligible && preparing_deadline.active
               && preparing_deadline.deadline
               && milliseconds(*preparing_deadline.deadline) == 2'500,
           "preparing cursor never schedules before frozen capture time");

    auto settled = preparing;
    settled.revision = 2;
    settled.state = LocalActivityPulseState::settled;
    settled.result_sha256 = repeated('d');
    expect(valid_local_activity_pulse_cursor(settled),
           "settled deadline fixture is canonical");
    const auto settled_deadline = inspect_local_activity_pulse_deadline(settled, true);
    expect(settled_deadline.eligible && settled_deadline.active
               && settled_deadline.deadline
               && milliseconds(*settled_deadline.deadline)
                    == 2'500 + local_activity_pulse_cadence_ms,
           "settled generation re-arms exactly 24h after capture");

    auto blocked = preparing;
    blocked.revision = 3;
    blocked.state = LocalActivityPulseState::blocked;
    blocked.blocked_reason = "synthetic invariant conflict";
    expect(valid_local_activity_pulse_cursor(blocked),
           "blocked deadline fixture is canonical");
    const auto blocked_deadline = inspect_local_activity_pulse_deadline(blocked, true);
    expect(blocked_deadline.eligible && !blocked_deadline.active
               && !blocked_deadline.deadline,
           "blocked cursor schedules no deadline");

    auto quiescent = settled;
    quiescent.revision = 3;
    quiescent.generation = 3;
    quiescent.state = LocalActivityPulseState::quiescent;
    quiescent.predecessor_observation_task_id =
        "continuity.local-observation.v1:" + repeated('e');
    quiescent.predecessor_observation_result_sha256 = repeated('f');
    expect(valid_local_activity_pulse_cursor(quiescent),
           "quiescent deadline fixture is canonical");
    const auto quiescent_deadline =
        inspect_local_activity_pulse_deadline(quiescent, true);
    expect(quiescent_deadline.eligible && !quiescent_deadline.active
               && !quiescent_deadline.deadline,
           "generation-three quiescent cursor schedules no deadline");

    auto invalid = idle;
    invalid.anchor_checkpoint_result_sha256 = "not-a-hash";
    const auto invalid_deadline = inspect_local_activity_pulse_deadline(invalid, true);
    expect(!invalid_deadline.eligible && !invalid_deadline.active,
           "invalid cursor fails closed instead of scheduling");

    Scheduler scheduler;
    LocalActivityPulseSchedulerBridge bridge(scheduler);
    expect(bridge.arm(idle, true) == LocalActivityPulseSchedulerArmResult::scheduled,
           "first durable deadline arms existing Scheduler");
    expect(scheduler.next()
               && milliseconds(*scheduler.next()) == idle.due_at_ms,
           "Scheduler stores exact durable idle deadline");
    expect(bridge.arm(idle, true) == LocalActivityPulseSchedulerArmResult::unchanged,
           "re-arming identical durable cursor is idempotent");

    auto earlier = idle_cursor(500, 1'500);
    expect(bridge.arm(earlier, true) == LocalActivityPulseSchedulerArmResult::advanced,
           "earlier durable deadline advances Scheduler exactly once");
    expect(scheduler.next() && milliseconds(*scheduler.next()) == 1'500,
           "Scheduler now owns earlier exact deadline");

    expect(bridge.arm(std::nullopt, false)
               == LocalActivityPulseSchedulerArmResult::inactive,
           "inactive durable state does not stop Scheduler permanently");
    expect(scheduler.next() && milliseconds(*scheduler.next()) == 1'500,
           "inactive arm does not mutate an already-owned Scheduler deadline");

    if (failures != 0) {
        std::cerr << failures << " local activity scheduler bridge test(s) failed\n";
        return 1;
    }
    std::cout << "All local activity scheduler bridge tests passed\n";
    return 0;
}
