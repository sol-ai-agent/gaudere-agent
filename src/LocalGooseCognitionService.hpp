#ifndef GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_SERVICE_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_COGNITION_SERVICE_HPP

#include "LocalActivityPulseStore.hpp"
#include "LocalGooseCognition.hpp"
#include "LocalGooseCognitionHandler.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseCognitionServiceResult {
    no_settled_observation,
    submitted,
    waiting,
    succeeded,
    failed,
    conflict,
    unavailable
};

struct LocalGooseCognitionServiceStep {
    bool healthy = false;
    bool monitoring = false;
    LocalGooseCognitionServiceResult result =
        LocalGooseCognitionServiceResult::unavailable;
    std::optional<gaudere::work::Task> task;
    std::optional<LocalGooseDecision> decision;
    std::string detail;
};

/**
 * Provider-free bridge from the most recently settled local observation to one
 * deterministic local Goose cognition for that observation and model.
 *
 * The cursor reader is read-only. This service mutates only the normal Task
 * ledger through the existing work Runtime. A failed local model decision is a
 * durable learning/failure record and never blocks the pulse scheduler.
 */
class LocalGooseCognitionService {
public:
    using CursorReader =
        std::function<std::optional<LocalActivityPulseCursor>()>;

    LocalGooseCognitionService(
        CursorReader cursor_reader,
        gaudere::work::TaskStore& task_store,
        gaudere::work::Runtime& work_runtime,
        LocalGooseCognitionHandler& handler,
        std::string model_sha256);

    [[nodiscard]] LocalGooseCognitionServiceStep step();

private:
    CursorReader cursor_reader_;
    gaudere::work::TaskStore& task_store_;
    gaudere::work::Runtime& work_runtime_;
    LocalGooseCognitionHandler& handler_;
    std::string model_sha256_;
};

} // namespace gaudere_agent

#endif
