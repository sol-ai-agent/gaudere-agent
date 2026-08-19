#include "OpenAIOneShot.hpp"

#include "TaskReport.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace gaudere_agent {

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
