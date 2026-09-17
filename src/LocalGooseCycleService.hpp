#ifndef GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_SERVICE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_CYCLE_SERVICE_HPP

#include "LocalGooseCycleHandler.hpp"
#include "LocalGooseCycleStore.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

inline constexpr std::int64_t local_goose_cycle_minimum_delay_ms = 1'000;

enum class LocalGooseCycleServiceResult {
    dormant,
    waiting,
    prepared,
    submitted,
    executed,
    scheduled,
    blocked,
    conflict,
    unavailable
};

struct LocalGooseCycleServiceStep {
    bool healthy = false;
    bool active = false;
    LocalGooseCycleServiceResult result = LocalGooseCycleServiceResult::unavailable;
    std::optional<LocalGooseCycleCursor> cursor;
    std::optional<gaudere::work::Task> task;
    std::optional<LocalGooseDecision> decision;
    std::string detail;
};

/**
 * Dedicated provider-free runtime for cognition.local-goose-cycle.v1.
 *
 * It is deliberately not a TaskDispatcher handler. Durable scheduling truth lives
 * only in LocalGooseCycleStore. One call performs at most one cursor transition.
 * The first dormant generation-0 cursor is never activated by this service.
 */
class LocalGooseCycleService {
public:
    using NowMs = std::function<std::int64_t()>;

    LocalGooseCycleService(LocalGooseCycleStore& cycle_store,
                           gaudere::work::TaskStore& task_store,
                           gaudere::work::Runtime& work_runtime,
                           LocalGooseCycleHandler& handler,
                           std::string model_sha256,
                           NowMs now_ms);

    [[nodiscard]] LocalGooseCycleServiceStep step();

private:
    LocalGooseCycleStore& cycle_store_;
    gaudere::work::TaskStore& task_store_;
    gaudere::work::Runtime& work_runtime_;
    LocalGooseCycleHandler& handler_;
    std::string model_sha256_;
    NowMs now_ms_;
};

} // namespace gaudere_agent

#endif
