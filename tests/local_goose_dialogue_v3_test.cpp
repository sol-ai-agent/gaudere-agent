#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV3.hpp"

#include "../src/Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;
using Task = gaudere::work::Task;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(const char value)
{
    return std::string(64, value);
}

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

bool throws_root(const std::string& request_id,
                 const std::string& speaker_kind,
                 const std::string& speaker_id,
                 const std::string& message_kind,
                 const std::string& message,
                 const std::string& model)
{
    try {
        static_cast<void>(make_local_goose_dialogue_v3_root_task(
            request_id, speaker_kind, speaker_id, message_kind, message, model));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

} // namespace

int main()
{
    const auto model = hex('a');

    const auto native_root = make_local_goose_dialogue_v3_root_task(
        "v3-native-root",
        local_goose_dialogue_v3_speaker_human,
        "bertrand",
        local_goose_dialogue_v3_message_dialogue,
        "Bonjour V3.",
        model);
    const auto native_retry = make_local_goose_dialogue_v3_root_task(
        "v3-native-root",
        local_goose_dialogue_v3_speaker_human,
        "bertrand",
        local_goose_dialogue_v3_message_dialogue,
        "Bonjour V3.",
        model);
    assert(native_root.id == native_retry.id);
    assert(native_root.idempotency_key == native_retry.idempotency_key);
    assert(native_root.kind == local_goose_dialogue_v3_task_kind);
    assert(native_root.input_content_type == local_goose_dialogue_v3_content_type);

    const auto native_inspection =
        inspect_local_goose_dialogue_v3_task(native_root);
    assert(native_inspection.eligible);
    assert(native_inspection.request_id == "v3-native-root");
    assert(native_inspection.speaker_kind == "human");
    assert(native_inspection.speaker_id == "bertrand");
    assert(native_inspection.message_kind == "dialogue");
    assert(native_inspection.turn_index == 0);
    assert(native_inspection.root_task_id.empty());
    assert(native_inspection.predecessor_task_id.empty());
    assert(native_inspection.predecessor_result_sha256.empty());

    const auto changed_actor = make_local_goose_dialogue_v3_root_task(
        "v3-native-root",
        local_goose_dialogue_v3_speaker_system,
        "sol",
        local_goose_dialogue_v3_message_dialogue,
        "Bonjour V3.",
        model);
    assert(changed_actor.id != native_root.id);
    assert(changed_actor.idempotency_key == native_root.idempotency_key);

    assert(throws_root(
        "", "human", "bertrand", "dialogue", "message", model));
    assert(throws_root(
        "bad id", "human", "bertrand", "dialogue", "message", model));
    assert(throws_root(
        "request", "assistant", "bertrand", "dialogue", "message", model));
    assert(throws_root(
        "request", "human", "bad id", "dialogue", "message", model));
    assert(throws_root(
        "request", "human", "bertrand", "command", "message", model));
    assert(throws_root(
        "request", "human", "bertrand", "dialogue", "", model));
    assert(throws_root(
        "request", "human", "bertrand", "dialogue",
        std::string(4097, 'x'), model));
    assert(throws_root(
        "request", "human", "bertrand", "dialogue", "message", "bad-hash"));

    const auto native_succeeded = succeed_v3(native_root, "Bonjour Bertrand.");
    const auto native_response = inspect_local_goose_dialogue_v3_response(
        native_succeeded, native_succeeded.result->output);
    assert(native_response.eligible);
    assert(native_response.speaker_kind == "human");
    assert(native_response.speaker_id == "bertrand");
    assert(native_response.message_kind == "dialogue");
    assert(native_response.root_task_id == native_root.id);

    const auto v2_root = make_local_goose_dialogue_v2_root_task(
        "legacy-root", "Bonjour.", model);
    const auto v2_root_succeeded = succeed_v2(v2_root, "Bonjour.");
    const auto v2_turn1 = make_local_goose_dialogue_v2_successor_task(
        "legacy-turn-1", "Deuxième tour.", model, v2_root_succeeded);
    const auto v2_turn1_succeeded = succeed_v2(
        v2_turn1, "Oui, je conserve le contexte.");

    const auto bridge = make_local_goose_dialogue_v3_bridge_from_v2_task(
        "v3-bridge-turn-2",
        local_goose_dialogue_v3_speaker_system,
        "sol",
        local_goose_dialogue_v3_message_observation,
        "Le système rejoint explicitement ce fil.",
        model,
        v2_turn1_succeeded);
    const auto bridge_retry = make_local_goose_dialogue_v3_bridge_from_v2_task(
        "v3-bridge-turn-2",
        local_goose_dialogue_v3_speaker_system,
        "sol",
        local_goose_dialogue_v3_message_observation,
        "Le système rejoint explicitement ce fil.",
        model,
        v2_turn1_succeeded);
    assert(bridge.id == bridge_retry.id);

    const auto bridge_inspection =
        inspect_local_goose_dialogue_v3_task(bridge);
    assert(bridge_inspection.eligible);
    assert(bridge_inspection.speaker_kind == "system");
    assert(bridge_inspection.speaker_id == "sol");
    assert(bridge_inspection.message_kind == "observation");
    assert(bridge_inspection.turn_index == 2);
    assert(bridge_inspection.root_task_id == v2_root.id);
    assert(bridge_inspection.predecessor_task_id == v2_turn1.id);
    assert(bridge_inspection.predecessor_result_sha256
        == sha256_hex(v2_turn1_succeeded.result->output));

    const auto bridge_succeeded = succeed_v3(
        bridge, "J'ai bien reçu cette observation du système.");
    const auto bridge_response = inspect_local_goose_dialogue_v3_response(
        bridge_succeeded, bridge_succeeded.result->output);
    assert(bridge_response.eligible);
    assert(bridge_response.root_task_id == v2_root.id);
    assert(bridge_response.turn_index == 2);
    assert(bridge_response.speaker_kind == "system");
    assert(bridge_response.speaker_id == "sol");
    assert(bridge_response.message_kind == "observation");

    const auto system_feedback = make_local_goose_dialogue_v3_successor_task(
        "v3-system-feedback-3",
        local_goose_dialogue_v3_speaker_system,
        "gaudere-runtime",
        local_goose_dialogue_v3_message_feedback,
        "Retour système: ta réponse précédente a été observée.",
        model,
        bridge_succeeded);
    const auto feedback_inspection =
        inspect_local_goose_dialogue_v3_task(system_feedback);
    assert(feedback_inspection.eligible);
    assert(feedback_inspection.turn_index == 3);
    assert(feedback_inspection.root_task_id == v2_root.id);
    assert(feedback_inspection.predecessor_task_id == bridge.id);
    assert(feedback_inspection.predecessor_result_sha256
        == sha256_hex(bridge_succeeded.result->output));
    assert(feedback_inspection.speaker_kind == "system");
    assert(feedback_inspection.speaker_id == "gaudere-runtime");
    assert(feedback_inspection.message_kind == "feedback");

    bool raw_v2_as_v3_successor = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v3_successor_task(
            "wrong-successor",
            "system",
            "sol",
            "feedback",
            "message",
            model,
            v2_turn1_succeeded));
    } catch (const std::invalid_argument&) {
        raw_v2_as_v3_successor = true;
    }
    assert(raw_v2_as_v3_successor);

    bool raw_v3_as_bridge = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v3_bridge_from_v2_task(
            "wrong-bridge",
            "system",
            "sol",
            "observation",
            "message",
            model,
            bridge_succeeded));
    } catch (const std::invalid_argument&) {
        raw_v3_as_bridge = true;
    }
    assert(raw_v3_as_bridge);

    bool wrong_model = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v3_bridge_from_v2_task(
            "wrong-model",
            "system",
            "sol",
            "observation",
            "message",
            hex('b'),
            v2_turn1_succeeded));
    } catch (const std::invalid_argument&) {
        wrong_model = true;
    }
    assert(wrong_model);

    auto tampered = system_feedback;
    tampered.input += " ";
    assert(!inspect_local_goose_dialogue_v3_task(tampered).eligible);

    tampered = system_feedback;
    tampered.limits.max_attempts = 3;
    assert(!inspect_local_goose_dialogue_v3_task(tampered).eligible);

    const auto feedback_succeeded =
        succeed_v3(system_feedback, "Merci pour ce retour.");
    auto bad_result = feedback_succeeded;
    bad_result.result->output += " ";
    assert(!canonical_local_goose_dialogue_v3_success(bad_result));

    auto wrong_type = feedback_succeeded;
    wrong_type.result->content_type = "text/plain";
    assert(!canonical_local_goose_dialogue_v3_success(wrong_type));

    bool response_too_large = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v3_response(
            feedback_inspection, std::string(16 * 1024 + 1, 'x')));
    } catch (const std::invalid_argument&) {
        response_too_large = true;
    }
    assert(response_too_large);

    std::cout << "local_goose_dialogue_v3_test: PASS\n";
    return 0;
}
