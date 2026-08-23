#include "WorkController.hpp"

#include <chrono>
#include <utility>

namespace gaudere_agent {

WorkController::WorkController(gaudere::scheduling::wake::Scheduler& scheduler,
                               gaudere::work::Runtime& runtime,
                               TaskDispatcher& dispatcher,
                               std::string worker,
                               gaudere::scheduling::wake::WakeIntentRuntime* wake_runtime)
    : scheduler_(scheduler),
      runtime_(runtime),
      dispatcher_(dispatcher),
      worker_(std::move(worker)),
      wake_runtime_(wake_runtime)
{
}

bool WorkController::start()
{
    bool expected = false;
    if (stopping_.load() || worker_.empty()
        || runtime_.state() != gaudere::work::RuntimeState::running
        || !started_.compare_exchange_strong(expected, true)) {
        return false;
    }
    reconcile_wakes();
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
    schedule_next_deadline();
    return true;
}

void WorkController::notify_work()
{
    if (!started_.load() || stopping_.load()) {
        return;
    }
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
}

void WorkController::interrupt()
{
    if (!started_.load() || stopping_.load()) {
        return;
    }
    scheduler_.interrupt();
}

void WorkController::refresh_deadlines()
{
    if (!started_.load() || stopping_.load()) {
        return;
    }
    schedule_next_deadline();
}

WorkCycleResult WorkController::wait_and_run()
{
    if (!started_.load()) {
        return WorkCycleResult::stopped;
    }
    if (stopping_.load()) {
        return enter_draining();
    }

    const auto wait_result = scheduler_.wait();
    if (wait_result == gaudere::scheduling::wake::WaitResult::stopped
        || stopping_.load()) {
        return enter_draining();
    }
    if (wait_result == gaudere::scheduling::wake::WaitResult::interrupted) {
        return WorkCycleResult::idle;
    }

    reconcile_wakes();
    static_cast<void>(runtime_.recover_expired());
    bool worked = false;
    for (;;) {
        if (stopping_.load()) {
            return enter_draining();
        }
        switch (dispatcher_.dispatch_one(
            worker_, [this] { return stopping_.load(); })) {
        case DispatchResult::dispatched:
            worked = true;
            if (stopping_.load()) {
                return enter_draining();
            }
            continue;
        case DispatchResult::idle:
            schedule_next_deadline();
            return worked ? WorkCycleResult::worked : WorkCycleResult::idle;
        case DispatchResult::state_conflict:
            schedule_next_deadline();
            return WorkCycleResult::state_conflict;
        }
    }
}

void WorkController::stop()
{
    stopping_.store(true);
    scheduler_.stop();
}

WorkCycleResult WorkController::enter_draining()
{
    runtime_.request_shutdown();
    return WorkCycleResult::stopped;
}

void WorkController::reconcile_wakes()
{
    if (wake_runtime_) {
        static_cast<void>(wake_runtime_->reconcile());
    }
}

void WorkController::schedule_next_deadline()
{
    if (stopping_.load()) {
        return;
    }
    if (const auto deadline = runtime_.next_recovery_at()) {
        static_cast<void>(scheduler_.request_at(*deadline));
    }
    if (wake_runtime_) {
        if (const auto deadline = wake_runtime_->next_scheduled_at()) {
            static_cast<void>(scheduler_.request_at(*deadline));
        }
    }
}

} // namespace gaudere_agent
