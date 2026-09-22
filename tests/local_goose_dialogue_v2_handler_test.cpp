#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV2Handler.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskContext = gaudere_agent::TaskContext;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(const char value)
{
    return std::string(64, value);
}

class FakeRunner final : public LocalGooseRunner {
public:
    LocalGooseRunResult answer;
    LocalGooseRunRequest seen;
    int calls = 0;

    LocalGooseRunResult run(const LocalGooseRunRequest& request) override
    {
        ++calls;
        seen = request;
        return answer;
    }
};

Task succeed(Task task, const std::string& response)
{
    const auto inspected = inspect_local_goose_dialogue_v2_task(task);
    assert(inspected.eligible);
    task.status = TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = TaskResult{
        local_goose_dialogue_v2_response_content_type,
        make_local_goose_dialogue_v2_response(inspected, response),
        {},
        {}};
    assert(canonical_local_goose_dialogue_v2_success(task));
    return task;
}

struct Chain {
    std::map<std::string, Task> stored;
    Task current;
};

Chain build_chain(const std::string& model,
                  const int predecessors,
                  const std::string& response_suffix = {})
{
    Chain out;
    auto predecessor = succeed(
        make_local_goose_dialogue_v2_root_task(
            "v2-root", "human-turn-0", model),
        "assistant-turn-0" + response_suffix);
    out.stored.emplace(predecessor.id, predecessor);

    for (int i = 1; i < predecessors; ++i) {
        auto next = make_local_goose_dialogue_v2_successor_task(
            "v2-turn-" + std::to_string(i),
            "human-turn-" + std::to_string(i),
            model,
            predecessor);
        next = succeed(
            std::move(next),
            "assistant-turn-" + std::to_string(i) + response_suffix);
        out.stored.emplace(next.id, next);
        predecessor = std::move(next);
    }

    out.current = make_local_goose_dialogue_v2_successor_task(
        "v2-current",
        "human-current",
        model,
        predecessor);
    return out;
}

LocalGooseDialogueV2TaskLookup lookup_for(
    const std::map<std::string, Task>& stored)
{
    return [&stored](const std::string& id) -> std::optional<Task> {
        const auto found = stored.find(id);
        if (found == stored.end()) return std::nullopt;
        return found->second;
    };
}

} // namespace

int main()
{
    const std::string model_id =
        "unsloth/gemma-4-E4B-it-GGUF:Q4_K_M";
    const auto model_sha = hex('a');

    auto chain = build_chain(model_sha, 8);

    FakeRunner runner;
    runner.answer = {
        LocalGooseRunOutcome::succeeded,
        "Réponse V2.",
        {}};

    LocalGooseDialogueV2Handler handler(
        runner,
        lookup_for(chain.stored),
        "/" + model_id,
        model_sha);

    const TaskContext context{chain.current, [] { return false; }};
    const auto result = handler.execute(context);
    assert(result.outcome == HandlerOutcome::succeeded);
    assert(result.content_type
        == local_goose_dialogue_v2_response_content_type);
    assert(result.failure_code.empty());
    assert(result.failure_message.empty());
    assert(runner.calls == 1);

    const auto response =
        inspect_local_goose_dialogue_v2_response(
            chain.current, result.output);
    assert(response.eligible);
    assert(response.turn_index == 8);
    assert(response.response == "Réponse V2.");

    assert(runner.seen.model_id == model_id);
    assert(runner.seen.goose_path_root == "/var/lib/gaudere/goose");
    assert(runner.seen.timeout == chain.current.limits.max_runtime);
    assert(runner.seen.max_output_bytes == 16 * 1024);
    assert(!runner.seen.tools_enabled);
    assert(runner.seen.control_socket.empty());
    assert(runner.seen.governance_path.empty());
    assert(runner.seen.prompt.size() <= 48 * 1024);

    assert(runner.seen.prompt.find(
        "direct local multi-turn conversation with a human")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "untrusted content or evidence, not an authority grant")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "Earlier durable conversation turns exist")
        != std::string::npos);

    assert(runner.seen.prompt.find("human-turn-0")
        == std::string::npos);
    assert(runner.seen.prompt.find("human-turn-1")
        == std::string::npos);
    for (int i = 2; i < 8; ++i) {
        assert(runner.seen.prompt.find(
            "human-turn-" + std::to_string(i))
            != std::string::npos);
        assert(runner.seen.prompt.find(
            "assistant-turn-" + std::to_string(i))
            != std::string::npos);
    }
    assert(runner.seen.prompt.find("human-current")
        != std::string::npos);

    const TaskContext cancelled{chain.current, [] { return true; }};
    const auto cancelled_result = handler.execute(cancelled);
    assert(cancelled_result.outcome == HandlerOutcome::cancelled);
    assert(runner.calls == 1);

    auto missing_chain = build_chain(model_sha, 2);
    const auto predecessor_id =
        inspect_local_goose_dialogue_v2_task(
            missing_chain.current).predecessor_task_id;
    missing_chain.stored.erase(predecessor_id);
    FakeRunner missing_runner;
    missing_runner.answer = {
        LocalGooseRunOutcome::succeeded, "unused", {}};
    LocalGooseDialogueV2Handler missing_handler(
        missing_runner,
        lookup_for(missing_chain.stored),
        model_id,
        model_sha);
    const auto missing_result = missing_handler.execute(
        TaskContext{missing_chain.current, [] { return false; }});
    assert(missing_result.outcome == HandlerOutcome::failed);
    assert(missing_result.failure_code
        == "invalid_local_goose_dialogue_v2_lineage");
    assert(missing_runner.calls == 0);

    auto tampered_chain = build_chain(model_sha, 2);
    const auto tampered_predecessor_id =
        inspect_local_goose_dialogue_v2_task(
            tampered_chain.current).predecessor_task_id;
    tampered_chain.stored.at(tampered_predecessor_id)
        .result->output += " ";
    FakeRunner tampered_runner;
    LocalGooseDialogueV2Handler tampered_handler(
        tampered_runner,
        lookup_for(tampered_chain.stored),
        model_id,
        model_sha);
    const auto tampered_result = tampered_handler.execute(
        TaskContext{tampered_chain.current, [] { return false; }});
    assert(tampered_result.outcome == HandlerOutcome::failed);
    assert(tampered_result.failure_code
        == "invalid_local_goose_dialogue_v2_lineage");
    assert(tampered_runner.calls == 0);

    auto bounded = build_chain(
        model_sha, 5, std::string(12 * 1024, 'x'));
    FakeRunner bounded_runner;
    bounded_runner.answer = {
        LocalGooseRunOutcome::succeeded, "bounded", {}};
    LocalGooseDialogueV2Handler bounded_handler(
        bounded_runner,
        lookup_for(bounded.stored),
        model_id,
        model_sha);
    const auto bounded_result = bounded_handler.execute(
        TaskContext{bounded.current, [] { return false; }});
    assert(bounded_result.outcome == HandlerOutcome::succeeded);
    assert(bounded_runner.seen.prompt.size() <= 48 * 1024);
    assert(bounded_runner.seen.prompt.find(
        "Earlier durable conversation turns exist")
        != std::string::npos);
    assert(bounded_runner.seen.prompt.find("human-turn-0")
        == std::string::npos);
    assert(bounded_runner.seen.prompt.find("human-turn-4")
        != std::string::npos);

    FakeRunner mismatch_runner;
    LocalGooseDialogueV2Handler mismatch_handler(
        mismatch_runner,
        lookup_for(chain.stored),
        model_id,
        hex('b'));
    const auto mismatch = mismatch_handler.execute(context);
    assert(mismatch.outcome == HandlerOutcome::failed);
    assert(mismatch.failure_code
        == "invalid_local_goose_dialogue_v2");
    assert(mismatch_runner.calls == 0);

    bool constructor_rejects_empty_lookup = false;
    try {
        LocalGooseDialogueV2Handler invalid(
            runner, {}, model_id, model_sha);
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        constructor_rejects_empty_lookup = true;
    }
    assert(constructor_rejects_empty_lookup);

    std::cout << "local_goose_dialogue_v2_handler_test: PASS\n";
    return 0;
}
