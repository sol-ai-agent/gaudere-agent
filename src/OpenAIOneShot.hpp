#ifndef GAUDERE_AGENT_OPENAI_ONE_SHOT_HPP
#define GAUDERE_AGENT_OPENAI_ONE_SHOT_HPP

#include "OpenAITask.hpp"
#include "WorkController.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <string>

namespace gaudere_agent {

/** Submit/reuse one durable OpenAI task and drive the existing WorkController
 * until the task reaches a terminal state, then print its report and stop the
 * controller cleanly.
 *
 * This helper does not define any retry policy. If a replacement process reaches
 * a second Task attempt after a crash, ProviderTaskHandler's existing Action rule
 * prevents another provider invocation and reconciles to manual review instead.
 */
void run_openai_once(gaudere::work::Runtime& runtime,
                     gaudere::work::TaskStore& store,
                     WorkController& controller,
                     std::string id,
                     std::string input);

} // namespace gaudere_agent

#endif
