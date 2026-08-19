#ifndef GAUDERE_AGENT_OPENAI_TASK_HPP
#define GAUDERE_AGENT_OPENAI_TASK_HPP

#include <gaudere/work/Task.hpp>

#include <string>

namespace gaudere_agent {

inline constexpr const char* openai_task_kind = "provider.openai.responses";

[[nodiscard]] gaudere::work::Task make_openai_task(std::string id,
                                                    std::string input);

} // namespace gaudere_agent

#endif
