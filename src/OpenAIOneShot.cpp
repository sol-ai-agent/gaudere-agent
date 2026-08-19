#include "OpenAIOneShot.hpp"

#include "TaskReport.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
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
    // Attempt two is reconciliation only. ProviderTaskHandler will observe the
    // pre-existing provider Action and will not invoke the provider a second time.
    task.limits.max_attempts = 2;
    return task;
}

void run_openai_once(gaudere::work::Runtime& runtime,
                     gaudere::work::TaskStore& store,
                     WorkController& controller,
                     std::string id,
                     std::string input)
{
    const auto submit = runtime.submit(make_openai_task(id, std::move(input)));
    if (submit != gaudere::work::SubmitResult::accepted
        && submit != gaudere::work::SubmitResult::duplicate) {
        throw std::runtime_error("OpenAI one-shot task submission rejected");
    }

    controller.notify_work();
    for (;;) {
        const auto task = store.find(id);
        if (!task) {
            throw std::runtime_error("OpenAI one-shot task disappeared from durable store");
        }
        if (gaudere::work::is_terminal(task->status)) {
            print_task_report(std::cout, *task);
            break;
        }

        const auto cycle = controller.wait_and_run();
        if (cycle == WorkCycleResult::state_conflict) {
            throw std::runtime_error("OpenAI one-shot work controller state conflict");
        }
        if (cycle == WorkCycleResult::stopped) {
            throw std::runtime_error("OpenAI one-shot controller stopped before task became terminal");
        }
    }

    controller.stop();
    if (controller.wait_and_run() != WorkCycleResult::stopped) {
        throw std::runtime_error("OpenAI one-shot controller did not enter draining");
    }
}

} // namespace gaudere_agent
