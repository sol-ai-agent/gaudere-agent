#ifndef GAUDERE_AGENT_BOUNDED_REFLECTION_HPP
#define GAUDERE_AGENT_BOUNDED_REFLECTION_HPP

#include "TaskExecutor.hpp"

#include <gaudere/work/Task.hpp>

#include <string>

namespace gaudere_agent {

inline constexpr const char* bounded_reflection_task_kind =
    "cognition.reflect.v1";
inline constexpr const char* bounded_reflection_decision_content_type =
    "application/vnd.gaudere.cognition-decision+json";

/** Build one explicitly submitted, single-call bounded reflection task. */
[[nodiscard]] gaudere::work::Task make_bounded_reflection_task(
    std::string id,
    std::string objective);

/** Validate and normalize the definite output of an existing provider handler.
 *
 * This wrapper never invokes anything except the borrowed provider handler. A
 * valid `propose_wake` decision remains durable output only; this class has no
 * scheduler, Runtime, or TaskStore access and therefore cannot create work.
 */
class BoundedReflectionHandler final : public TaskHandler {
public:
    explicit BoundedReflectionHandler(TaskHandler& provider_handler) noexcept;

    [[nodiscard]] HandlerResult execute(const TaskContext& context) override;

private:
    TaskHandler& provider_handler_;
};

} // namespace gaudere_agent

#endif
