#include "LocalGooseDialogue.hpp"

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

bool throws_make(const std::string& request_id,
                 const std::string& message,
                 const std::string& model)
{
    try {
        static_cast<void>(make_local_goose_dialogue_task(
            request_id, message, model));
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

} // namespace

int main()
{
    const auto model = hex('a');
    const auto task = make_local_goose_dialogue_task(
        "dialogue-001", "Bonjour Gaudere.", model);
    const auto duplicate = make_local_goose_dialogue_task(
        "dialogue-001", "Bonjour Gaudere.", model);

    assert(task.id == duplicate.id);
    assert(task.idempotency_key == duplicate.idempotency_key);
    assert(task.kind == local_goose_dialogue_task_kind);
    assert(task.input_content_type == local_goose_dialogue_content_type);

    const auto inspected = inspect_local_goose_dialogue_task(task);
    assert(inspected.eligible);
    assert(inspected.request_id == "dialogue-001");
    assert(inspected.message == "Bonjour Gaudere.");
    assert(inspected.model_sha256 == model);
    assert(inspected.message_sha256.size() == 64);
    assert(inspected.canonical_input == task.input);

    const auto changed_message = make_local_goose_dialogue_task(
        "dialogue-001", "Bonjour autrement.", model);
    assert(changed_message.id != task.id);
    assert(changed_message.idempotency_key == task.idempotency_key);

    const auto changed_model = make_local_goose_dialogue_task(
        "dialogue-001", "Bonjour Gaudere.", hex('b'));
    assert(changed_model.id != task.id);
    assert(changed_model.idempotency_key == task.idempotency_key);

    const auto changed_request = make_local_goose_dialogue_task(
        "dialogue-002", "Bonjour Gaudere.", model);
    assert(changed_request.id != task.id);
    assert(changed_request.idempotency_key != task.idempotency_key);

    auto tampered = task;
    tampered.input += " ";
    assert(!inspect_local_goose_dialogue_task(tampered).eligible);

    tampered = task;
    tampered.limits.max_attempts = 3;
    assert(!inspect_local_goose_dialogue_task(tampered).eligible);

    assert(throws_make("", "message", model));
    assert(throws_make("bad id", "message", model));
    assert(throws_make("dialogue", "", model));
    assert(throws_make("dialogue", std::string(4097, 'x'), model));
    assert(throws_make("dialogue", "message", "bad-hash"));

    const auto response = make_local_goose_dialogue_response(
        inspected, "Bonjour. Je suis disponible pour discuter.");
    const auto response_inspection =
        inspect_local_goose_dialogue_response(task, response);
    assert(response_inspection.eligible);
    assert(response_inspection.request_id == inspected.request_id);
    assert(response_inspection.model_sha256 == model);
    assert(response_inspection.response
        == "Bonjour. Je suis disponible pour discuter.");

    auto succeeded = task;
    succeeded.status = TaskStatus::succeeded;
    succeeded.attempts_started = 1;
    succeeded.result = TaskResult{
        local_goose_dialogue_response_content_type,
        response,
        {},
        {}};
    assert(canonical_local_goose_dialogue_success(succeeded));

    auto bad_attempts = succeeded;
    bad_attempts.attempts_started = 0;
    assert(!canonical_local_goose_dialogue_success(bad_attempts));

    auto wrong_type = succeeded;
    wrong_type.result->content_type = "text/plain";
    assert(!canonical_local_goose_dialogue_success(wrong_type));

    auto wrong_request = succeeded;
    wrong_request.result->output =
        make_local_goose_dialogue_response(
            inspect_local_goose_dialogue_task(changed_request),
            "other");
    assert(!canonical_local_goose_dialogue_success(wrong_request));

    auto malformed = succeeded;
    malformed.result->output = "not-json";
    assert(!canonical_local_goose_dialogue_success(malformed));

    bool response_too_large = false;
    try {
        static_cast<void>(make_local_goose_dialogue_response(
            inspected, std::string(16 * 1024 + 1, 'x')));
    } catch (const std::invalid_argument&) {
        response_too_large = true;
    }
    assert(response_too_large);

    std::cout << "local_goose_dialogue_test: PASS\n";
    return 0;
}
