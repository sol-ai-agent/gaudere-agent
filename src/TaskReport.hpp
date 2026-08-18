#ifndef GAUDERE_AGENT_TASK_REPORT_HPP
#define GAUDERE_AGENT_TASK_REPORT_HPP

#include <gaudere/work/Task.hpp>

#include <iosfwd>
#include <string>

namespace gaudere_agent {

[[nodiscard]] const char* task_status_name(gaudere::work::TaskStatus status) noexcept;
[[nodiscard]] std::string escaped_text(const std::string& value);
void print_task_report(std::ostream& output, const gaudere::work::Task& task);

} // namespace gaudere_agent

#endif
