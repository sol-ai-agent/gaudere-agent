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
    if (started_ || stopping_ || worker_.empty()
        || runtime_.state() != gaudere::work::RuntimeState::running) {
        return false;
    }
    started_ = true;
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
    schedule_recovery_deadline();
    return true;
}

void WorkController::notify_work()
{
    if (!started_ || stopping_) {
        return;
    }
    static_cast<void>(scheduler_.request_after(std::chrono::seconds{0}));
}

WorkCycleResult WorkController::wait_and_run()
{
    if (!started_ || stopping_) {
        return WorkCycleResult::stopped;
    }

    if (scheduler_.wait() == gaudere::scheduling::wake::WaitResult::stopped) {
        stopping_ = true;
        return WorkCycleResult::stopped;
    }
    if (stopping_) {
        return WorkCycleResult::stopped;
    }

    static_cast<void>(runtime_.recover_expired());
    bool worked = false;
    for (;;) {
        switch (dispatcher_.dispatch_one(worker_)) {
        case DispatchResult::dispatched:
            worked = true;
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
    if (stopping_) {
        return;
    }
    stopping_ = true;
    scheduler_.stop();
    runtime_.request_shutdown();
}

void WorkController::schedule_recovery_deadline()
{
    if (stopping_) {
        return;
    }
    if (const auto deadline = runtime_.next_recovery_at()) {
        static_cast<void>(scheduler_.request_at(*deadline));
    }
}

} // namespace gaudere_agent
