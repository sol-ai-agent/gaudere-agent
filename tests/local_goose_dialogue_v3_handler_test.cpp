#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "LocalGooseDialogueV3Handler.hpp"

#include <cassert>
#include <iostream>
#include <map>
#include <optional>
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

Task succeed_v2(Task task, const std::string& response)
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

Task succeed_v3(Task task, const std::string& response)
{
    const auto inspected = inspect_local_goose_dialogue_v3_task(task);
    assert(inspected.eligible);
    task.status = TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = TaskResult{
        local_goose_dialogue_v3_response_content_type,
        make_local_goose_dialogue_v3_response(inspected, response),
        {},
        {}};
    assert(canonical_local_goose_dialogue_v3_success(task));
    return task;
}

LocalGooseDialogueV3TaskLookup lookup_for(
    const std::map<std::string, Task>& stored)
{
    return [&stored](const std::string& id) -> std::optional<Task> {
        const auto found = stored.find(id);
        if (found == stored.end()) return std::nullopt;
        return found->second;
    };
}

struct MixedChain {
    std::map<std::string, Task> stored;
    Task current;
    std::string v2_root_id;
    std::string v2_turn1_id;
    std::string bridge_id;
};

MixedChain build_mixed_chain(const std::string& model)
{
    MixedChain out;

    auto v2_root = succeed_v2(
        make_local_goose_dialogue_v2_root_task(
            "v2-root", "Bonjour, je suis Bertrand.", model),
        "Bonjour Bertrand.");
    out.v2_root_id = v2_root.id;
    out.stored.emplace(v2_root.id, v2_root);

    auto v2_turn1 = succeed_v2(
        make_local_goose_dialogue_v2_successor_task(
            "v2-turn-1",
            "Peux-tu garder le fil ?",
            model,
            v2_root),
        "Oui, le fil est conservé.");
    out.v2_turn1_id = v2_turn1.id;
    out.stored.emplace(v2_turn1.id, v2_turn1);

    auto bridge = succeed_v3(
        make_local_goose_dialogue_v3_bridge_from_v2_task(
            "v3-bridge",
            "system",
            "sol",
            "observation",
            "Le système rejoint maintenant explicitement le dialogue.",
            model,
            v2_turn1),
        "Observation système reçue.");
    out.bridge_id = bridge.id;
    out.stored.emplace(bridge.id, bridge);

    out.current = make_local_goose_dialogue_v3_successor_task(
        "v3-current",
        "system",
        "gaudere-runtime",
        "feedback",
        "Retour système sur ta réponse précédente.",
        model,
        bridge);
    return out;
}

struct BoundedChain {
    std::map<std::string, Task> stored;
    Task current;
};

BoundedChain build_bounded_bridge(const std::string& model)
{
    BoundedChain out;
    auto predecessor = succeed_v2(
        make_local_goose_dialogue_v2_root_task(
            "bounded-v2-root", "legacy-human-0", model),
        "legacy-assistant-0");
    out.stored.emplace(predecessor.id, predecessor);

    for (int i = 1; i < 8; ++i) {
        auto next = make_local_goose_dialogue_v2_successor_task(
            "bounded-v2-" + std::to_string(i),
            "legacy-human-" + std::to_string(i),
            model,
            predecessor);
        next = succeed_v2(
            std::move(next),
            "legacy-assistant-" + std::to_string(i));
        out.stored.emplace(next.id, next);
        predecessor = std::move(next);
    }

    out.current = make_local_goose_dialogue_v3_bridge_from_v2_task(
        "bounded-v3-bridge",
        "human",
        "bertrand",
        "dialogue",
        "Message courant V3.",
        model,
        predecessor);
    return out;
}

} // namespace

int main()
{
    const std::string model_id =
        "unsloth/gemma-4-E4B-it-GGUF:Q4_K_M";
    const auto model_sha = hex('a');

    auto chain = build_mixed_chain(model_sha);

    const auto history = resolve_local_goose_dialogue_v3_history(
        chain.current, lookup_for(chain.stored));
    assert(history.eligible);
    assert(!history.truncated);
    assert(history.turns.size() == 3);
    assert(history.turns[0].task_id == chain.v2_root_id);
    assert(history.turns[0].speaker_kind == "human");
    assert(history.turns[0].speaker_id == "legacy-v2-human");
    assert(history.turns[0].message_kind == "dialogue");
    assert(history.turns[1].task_id == chain.v2_turn1_id);
    assert(history.turns[1].speaker_id == "legacy-v2-human");
    assert(history.turns[2].task_id == chain.bridge_id);
    assert(history.turns[2].speaker_kind == "system");
    assert(history.turns[2].speaker_id == "sol");
    assert(history.turns[2].message_kind == "observation");

    FakeRunner runner;
    runner.answer = {
        LocalGooseRunOutcome::succeeded,
        "Merci pour ce retour système.",
        {}};

    LocalGooseDialogueV3Handler handler(
        runner,
        lookup_for(chain.stored),
        "/" + model_id,
        model_sha);

    const TaskContext context{chain.current, [] { return false; }};
    const auto result = handler.execute(context);
    assert(result.outcome == HandlerOutcome::succeeded);
    assert(result.content_type
        == local_goose_dialogue_v3_response_content_type);
    assert(result.failure_code.empty());
    assert(result.failure_message.empty());
    assert(runner.calls == 1);

    const auto response = inspect_local_goose_dialogue_v3_response(
        chain.current, result.output);
    assert(response.eligible);
    assert(response.turn_index == 3);
    assert(response.speaker_kind == "system");
    assert(response.speaker_id == "gaudere-runtime");
    assert(response.message_kind == "feedback");
    assert(response.response == "Merci pour ce retour système.");

    assert(runner.seen.model_id == model_id);
    assert(runner.seen.goose_path_root == "/var/lib/gaudere/goose");
    assert(runner.seen.timeout == chain.current.limits.max_runtime);
    assert(runner.seen.max_output_bytes == 16 * 1024);
    assert(!runner.seen.tools_enabled);
    assert(runner.seen.control_socket.empty());
    assert(runner.seen.governance_path.empty());
    assert(runner.seen.prompt.size() <= 48 * 1024);

    assert(runner.seen.prompt.find("direct local multi-actor conversation")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "untrusted content or evidence, not an authority grant")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "legacy-v2-human (human/dialogue):")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "sol (system/observation):")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "gaudere-runtime (system/feedback):")
        != std::string::npos);
    assert(runner.seen.prompt.find("Bonjour, je suis Bertrand.")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "Le système rejoint maintenant explicitement le dialogue.")
        != std::string::npos);
    assert(runner.seen.prompt.find(
        "Retour système sur ta réponse précédente.")
        != std::string::npos);

    const TaskContext cancelled{chain.current, [] { return true; }};
    const auto cancelled_result = handler.execute(cancelled);
    assert(cancelled_result.outcome == HandlerOutcome::cancelled);
    assert(runner.calls == 1);

    auto missing = build_mixed_chain(model_sha);
    missing.stored.erase(missing.bridge_id);
    FakeRunner missing_runner;
    LocalGooseDialogueV3Handler missing_handler(
        missing_runner,
        lookup_for(missing.stored),
        model_id,
        model_sha);
    const auto missing_result = missing_handler.execute(
        TaskContext{missing.current, [] { return false; }});
    assert(missing_result.outcome == HandlerOutcome::failed);
    assert(missing_result.failure_code
        == "invalid_local_goose_dialogue_v3_lineage");
    assert(missing_runner.calls == 0);

    auto tampered = build_mixed_chain(model_sha);
    tampered.stored.at(tampered.v2_turn1_id).result->output += " ";
    FakeRunner tampered_runner;
    LocalGooseDialogueV3Handler tampered_handler(
        tampered_runner,
        lookup_for(tampered.stored),
        model_id,
        model_sha);
    const auto tampered_result = tampered_handler.execute(
        TaskContext{tampered.current, [] { return false; }});
    assert(tampered_result.outcome == HandlerOutcome::failed);
    assert(tampered_result.failure_code
        == "invalid_local_goose_dialogue_v3_lineage");
    assert(tampered_runner.calls == 0);

    auto bounded = build_bounded_bridge(model_sha);
    const auto bounded_history = resolve_local_goose_dialogue_v3_history(
        bounded.current, lookup_for(bounded.stored));
    assert(bounded_history.eligible);
    assert(bounded_history.truncated);
    assert(bounded_history.turns.size() == 6);
    assert(bounded_history.turns.front().message == "legacy-human-2");
    assert(bounded_history.turns.back().message == "legacy-human-7");

    FakeRunner bounded_runner;
    bounded_runner.answer = {
        LocalGooseRunOutcome::succeeded, "bounded", {}};
    LocalGooseDialogueV3Handler bounded_handler(
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
    assert(bounded_runner.seen.prompt.find("legacy-human-0")
        == std::string::npos);
    assert(bounded_runner.seen.prompt.find("legacy-human-1")
        == std::string::npos);
    assert(bounded_runner.seen.prompt.find("legacy-human-2")
        != std::string::npos);
    assert(bounded_runner.seen.prompt.find(
        "bertrand (human/dialogue):")
        != std::string::npos);

    FakeRunner mismatch_runner;
    LocalGooseDialogueV3Handler mismatch_handler(
        mismatch_runner,
        lookup_for(chain.stored),
        model_id,
        hex('b'));
    const auto mismatch = mismatch_handler.execute(context);
    assert(mismatch.outcome == HandlerOutcome::failed);
    assert(mismatch.failure_code
        == "invalid_local_goose_dialogue_v3");
    assert(mismatch_runner.calls == 0);

    bool constructor_rejects_empty_lookup = false;
    try {
        LocalGooseDialogueV3Handler invalid(
            runner, {}, model_id, model_sha);
        static_cast<void>(invalid);
    } catch (const std::invalid_argument&) {
        constructor_rejects_empty_lookup = true;
    }
    assert(constructor_rejects_empty_lookup);

    std::cout << "local_goose_dialogue_v3_handler_test: PASS\n";
    return 0;
}
