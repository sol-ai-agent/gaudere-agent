#include "LocalGooseDialogueV2.hpp"

#include "../src/Sha256.hpp"

#include <gaudere/work/Task.hpp>

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;
using TaskResult = gaudere::work::TaskResult;
using TaskStatus = gaudere::work::TaskStatus;

std::string hex(const char value)
{
    return std::string(64, value);
}

bool throws_root(const std::string& request_id,
                 const std::string& message,
                 const std::string& model)
{
    try {
        static_cast<void>(make_local_goose_dialogue_v2_root_task(
            request_id, message, model));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

gaudere::work::Task succeed(
    gaudere::work::Task task,
    const std::string& response)
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

} // namespace

int main()
{
    const auto model = hex('a');

    const auto root = make_local_goose_dialogue_v2_root_task(
        "thread-root-001", "Bonjour Gaudere.", model);
    const auto root_retry = make_local_goose_dialogue_v2_root_task(
        "thread-root-001", "Bonjour Gaudere.", model);
    assert(root.id == root_retry.id);
    assert(root.idempotency_key == root_retry.idempotency_key);
    assert(root.kind == local_goose_dialogue_v2_task_kind);
    assert(root.input_content_type == local_goose_dialogue_v2_content_type);

    const auto root_inspection = inspect_local_goose_dialogue_v2_task(root);
    assert(root_inspection.eligible);
    assert(root_inspection.task_id == root.id);
    assert(root_inspection.request_id == "thread-root-001");
    assert(root_inspection.message == "Bonjour Gaudere.");
    assert(root_inspection.model_sha256 == model);
    assert(root_inspection.turn_index == 0);
    assert(root_inspection.root_task_id.empty());
    assert(root_inspection.predecessor_task_id.empty());
    assert(root_inspection.predecessor_result_sha256.empty());

    const auto changed_message = make_local_goose_dialogue_v2_root_task(
        "thread-root-001", "Bonjour autrement.", model);
    assert(changed_message.id != root.id);
    assert(changed_message.idempotency_key == root.idempotency_key);

    const auto changed_request = make_local_goose_dialogue_v2_root_task(
        "thread-root-002", "Bonjour Gaudere.", model);
    assert(changed_request.id != root.id);
    assert(changed_request.idempotency_key != root.idempotency_key);

    assert(throws_root("", "message", model));
    assert(throws_root("bad id", "message", model));
    assert(throws_root("thread", "", model));
    assert(throws_root("thread", std::string(4097, 'x'), model));
    assert(throws_root("thread", "message", "bad-hash"));

    const auto root_succeeded = succeed(root, "Bonjour Bertrand.");
    const auto root_response = inspect_local_goose_dialogue_v2_response(
        root_succeeded, root_succeeded.result->output);
    assert(root_response.eligible);
    assert(root_response.root_task_id == root.id);
    assert(root_response.turn_index == 0);
    assert(root_response.predecessor_task_id.empty());
    assert(root_response.predecessor_result_sha256.empty());

    const auto turn1 = make_local_goose_dialogue_v2_successor_task(
        "thread-turn-001", "Comment vas-tu ?", model, root_succeeded);
    const auto turn1_retry = make_local_goose_dialogue_v2_successor_task(
        "thread-turn-001", "Comment vas-tu ?", model, root_succeeded);
    assert(turn1.id == turn1_retry.id);
    assert(turn1.idempotency_key == turn1_retry.idempotency_key);

    const auto turn1_inspection = inspect_local_goose_dialogue_v2_task(turn1);
    assert(turn1_inspection.eligible);
    assert(turn1_inspection.turn_index == 1);
    assert(turn1_inspection.root_task_id == root.id);
    assert(turn1_inspection.predecessor_task_id == root.id);
    assert(turn1_inspection.predecessor_result_sha256
        == sha256_hex(root_succeeded.result->output));

    const auto branch = make_local_goose_dialogue_v2_successor_task(
        "thread-branch-001", "Et sur une autre branche ?", model, root_succeeded);
    const auto branch_inspection = inspect_local_goose_dialogue_v2_task(branch);
    assert(branch_inspection.eligible);
    assert(branch_inspection.root_task_id == root.id);
    assert(branch_inspection.predecessor_task_id == root.id);
    assert(branch.id != turn1.id);

    const auto turn1_succeeded = succeed(
        turn1, "Je fonctionne normalement.");
    const auto turn1_response = inspect_local_goose_dialogue_v2_response(
        turn1_succeeded, turn1_succeeded.result->output);
    assert(turn1_response.eligible);
    assert(turn1_response.root_task_id == root.id);
    assert(turn1_response.turn_index == 1);
    assert(turn1_response.predecessor_task_id == root.id);
    assert(turn1_response.predecessor_result_sha256
        == sha256_hex(root_succeeded.result->output));

    const auto turn2 = make_local_goose_dialogue_v2_successor_task(
        "thread-turn-002", "Très bien.", model, turn1_succeeded);
    const auto turn2_inspection = inspect_local_goose_dialogue_v2_task(turn2);
    assert(turn2_inspection.eligible);
    assert(turn2_inspection.turn_index == 2);
    assert(turn2_inspection.root_task_id == root.id);
    assert(turn2_inspection.predecessor_task_id == turn1.id);
    assert(turn2_inspection.predecessor_result_sha256
        == sha256_hex(turn1_succeeded.result->output));

    bool bad_predecessor = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v2_successor_task(
            "bad-pred", "message", model, root));
    } catch (const std::invalid_argument&) {
        bad_predecessor = true;
    }
    assert(bad_predecessor);

    bool wrong_model = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v2_successor_task(
            "wrong-model", "message", hex('b'), root_succeeded));
    } catch (const std::invalid_argument&) {
        wrong_model = true;
    }
    assert(wrong_model);

    auto tampered = turn1;
    tampered.input += " ";
    assert(!inspect_local_goose_dialogue_v2_task(tampered).eligible);

    tampered = turn1;
    tampered.limits.max_attempts = 3;
    assert(!inspect_local_goose_dialogue_v2_task(tampered).eligible);

    auto bad_result = turn1_succeeded;
    bad_result.result->output += " ";
    assert(!canonical_local_goose_dialogue_v2_success(bad_result));

    auto wrong_type = turn1_succeeded;
    wrong_type.result->content_type = "text/plain";
    assert(!canonical_local_goose_dialogue_v2_success(wrong_type));

    bool response_too_large = false;
    try {
        static_cast<void>(make_local_goose_dialogue_v2_response(
            root_inspection, std::string(16 * 1024 + 1, 'x')));
    } catch (const std::invalid_argument&) {
        response_too_large = true;
    }
    assert(response_too_large);

    std::cout << "local_goose_dialogue_v2_test: PASS\n";
    return 0;
}
