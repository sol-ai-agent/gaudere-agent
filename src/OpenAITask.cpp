#include "OpenAITask.hpp"

#include <chrono>
#include <utility>

namespace gaudere_agent {

gaudere::work::Task make_openai_task(std::string id, std::string input)
{
    gaudere::work::Task task;
    task.id = std::move(id);
    task.idempotency_key = "openai.responses:" + task.id;
    task.kind = openai_task_kind;
    task.input_content_type = "text/plain; charset=utf-8";
    task.input = std::move(input);
    task.limits.max_input_bytes = 16 * 1024;
    task.limits.max_output_bytes = 64 * 1024;
    task.limits.max_runtime = std::chrono::seconds{60};
    // Attempt two is reconciliation only. ProviderTaskHandler observes the existing
    // provider Action and never invokes the provider a second time.
    task.limits.max_attempts = 2;
    return task;
}

} // namespace gaudere_agent
