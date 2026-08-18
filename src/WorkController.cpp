#include "WorkController.hpp"

#include <chrono>
#include <utility>

namespace gaudere_agent {

WorkController::WorkController(gaudere::scheduling::wake::Scheduler& scheduler,
                               gaudere::work::Runtime& runtime,
                               TaskDispatcher& dispatcher,
                               std::string worker)
    : scheduler_(scheduler),
      runtime_(runtime),
      dispatcher_(dispatcher),
      worker_(std::move(worker))
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
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
    schedule_recovery_deadline();
    return true;
}

void WorkController::notify_work()
{
    if (!started_.load() || stopping_.load()) {
        return;
    }
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
}

WorkCycleResult WorkController::wait_and_run()
{
    if (!started_.load()) {
        return WorkCycleResult::stopped;
    }
    if (stopping_.load()) {
        return enter_draining();
    }

    if (scheduler_.wait() == gaudere::scheduling::wake::WaitResult::stopped
        || stopping_.load()) {
        return enter_draining();
    }

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
            schedule_recovery_deadline();
            return worked ? WorkCycleResult::worked : WorkCycleResult::idle;
        case DispatchResult::state_conflict:
            schedule_recovery_deadline();
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

void WorkController::schedule_recovery_deadline()
{
    if (stopping_.load()) {
        return;
    }
    if (const auto deadline = runtime_.next_recovery_at()) {
        static_cast<void>(scheduler_.request_at(*deadline));
    }
}

} // namespace gaudere_agent
