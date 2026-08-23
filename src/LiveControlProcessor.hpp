#ifndef GAUDERE_AGENT_LIVE_CONTROL_PROCESSOR_HPP
#define GAUDERE_AGENT_LIVE_CONTROL_PROCESSOR_HPP

#include "ExplicitWake.hpp"
#include "LiveControl.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstddef>
#include <functional>
#include <optional>

namespace gaudere_agent {

struct LiveControlProcessResult {
    std::size_t processed = 0;
    bool work_may_be_pending = false;
    bool wake_deadline_may_have_changed = false;
};

/**
 * Worker-thread side of the live control boundary.
 *
 * This is the only live-control object that knows Runtime/TaskStore/BudgetStore.
 * Call process() only from the same worker thread that owns normal Runtime/SQLite
 * transitions. The AF_UNIX listener has access only to LiveControlMailbox and the
 * scheduler wake callback, never to this processor or durable state.
 */
class LiveControlProcessor {
public:
    using SchedulerNext = std::function<
        std::optional<gaudere::scheduling::wake::WakeIntentTimePoint>()>;

    LiveControlProcessor(gaudere::work::Runtime& runtime,
                         gaudere::work::TaskStore& store,
                         gaudere::budget::Store& budget_store,
                         gaudere::budget::Policy budget_policy,
                         bool openai_enabled,
                         ExplicitWake* explicit_wake = nullptr,
                         SchedulerNext scheduler_next = {});

    [[nodiscard]] LiveControlProcessResult process(LiveControlMailbox& mailbox);

private:
    [[nodiscard]] LiveControlReply process_one(const LiveControlCommand& command,
                                               bool& work_may_be_pending,
                                               bool& wake_deadline_may_have_changed);

    gaudere::work::Runtime& runtime_;
    gaudere::work::TaskStore& store_;
    gaudere::budget::Store& budget_store_;
    gaudere::budget::Policy budget_policy_;
    bool openai_enabled_;
    ExplicitWake* explicit_wake_;
    SchedulerNext scheduler_next_;
};

} // namespace gaudere_agent

#endif
