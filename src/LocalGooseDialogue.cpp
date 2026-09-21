#include "LocalGooseDialogue.hpp"

#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

constexpr std::size_t max_request_id_bytes = 128;
constexpr std::size_t max_message_bytes = 4096;
constexpr std::size_t max_input_bytes = 8 * 1024;
constexpr std::size_t max_response_bytes = 16 * 1024;
constexpr std::size_t max_result_bytes = 24 * 1024;

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

bool safe_request_id(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_request_id_bytes) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool prefixed_sha256(const std::string& value, const char* prefix) noexcept
{
    const std::string p{prefix};
    return value.size() == p.size() + 64
        && value.compare(0, p.size(), p) == 0
        && lowercase_sha256(value.substr(p.size()));
}

bool same_limits(const gaudere::work::ResourceLimits& left,
                 const gaudere::work::ResourceLimits& right) noexcept
{
    return left.max_input_bytes == right.max_input_bytes
        && left.max_output_bytes == right.max_output_bytes
        && left.max_runtime == right.max_runtime
        && left.max_attempts == right.max_attempts;
}

std::string identity_material(
    const std::string& request_id,
    const std::string& message_sha256,
    const std::string& model_sha256)
{
    return std::string{"schema="} + local_goose_dialogue_schema + "\n"
        + "request_id=" + request_id + "\n"
        + "message_sha256=" + message_sha256 + "\n"
        + "model_sha256=" + model_sha256 + "\n";
}

} // namespace

Task make_local_goose_dialogue_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256)
{
    if (!safe_request_id(request_id)) {
        throw std::invalid_argument(
            "local Goose dialogue request id is invalid");
    }
    if (message.empty() || message.size() > max_message_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue message must be 1..4096 bytes");
    }
    if (!lowercase_sha256(model_sha256)) {
        throw std::invalid_argument(
            "local Goose dialogue model sha256 is invalid");
    }

    const Json input{
        {"message", message},
        {"model_sha256", model_sha256},
        {"request_id", request_id},
        {"schema", local_goose_dialogue_schema}
    };
    const auto encoded = input.dump();
    if (encoded.size() > max_input_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue canonical input exceeds bound");
    }

    const auto message_hash = sha256_hex(message);
    const auto identity = sha256_hex(identity_material(
        request_id, message_hash, model_sha256));

    Task task;
    task.id = std::string{local_goose_dialogue_task_prefix} + identity;
    // Request identity is single-use independently of message/model content.
    // Runtime duplicate detection can therefore distinguish a true retry from
    // reuse of the same request id with different bytes or model evidence.
    task.idempotency_key = std::string{local_goose_dialogue_task_prefix}
        + "request-id:" + sha256_hex(request_id);
    task.kind = local_goose_dialogue_task_kind;
    task.input_content_type = local_goose_dialogue_content_type;
    task.input = encoded;
    task.limits.max_input_bytes = max_input_bytes;
    task.limits.max_output_bytes = max_result_bytes;
    task.limits.max_runtime = std::chrono::minutes{10};
    task.limits.max_attempts = 2;
    return task;
}

LocalGooseDialogueInspection inspect_local_goose_dialogue_task(
    const Task& task) noexcept
{
    LocalGooseDialogueInspection out;
    try {
        if (!prefixed_sha256(task.id, local_goose_dialogue_task_prefix)
            || task.kind != local_goose_dialogue_task_kind
            || task.input_content_type != local_goose_dialogue_content_type
            || task.input.empty() || task.input.size() > max_input_bytes) {
            out.detail = "local Goose dialogue Task envelope is invalid";
            return out;
        }

        const auto parsed = Json::parse(task.input);
        const std::set<std::string> expected{
            "message", "model_sha256", "request_id", "schema"};
        std::set<std::string> keys;
        if (!parsed.is_object()) {
            out.detail = "local Goose dialogue input is not an object";
            return out;
        }
        for (const auto& item : parsed.items()) keys.insert(item.key());
        if (keys != expected
            || parsed.value("schema", "") != local_goose_dialogue_schema
            || !parsed.at("message").is_string()
            || !parsed.at("model_sha256").is_string()
            || !parsed.at("request_id").is_string()
            || parsed.dump() != task.input) {
            out.detail =
                "local Goose dialogue JSON schema/canonical bytes differ";
            return out;
        }

        out.request_id = parsed.at("request_id").get<std::string>();
        out.message = parsed.at("message").get<std::string>();
        out.model_sha256 = parsed.at("model_sha256").get<std::string>();
        if (!safe_request_id(out.request_id)
            || out.message.empty() || out.message.size() > max_message_bytes
            || !lowercase_sha256(out.model_sha256)) {
            out.detail = "local Goose dialogue fields are invalid";
            return out;
        }
        out.message_sha256 = sha256_hex(out.message);

        const auto identity = sha256_hex(identity_material(
            out.request_id, out.message_sha256, out.model_sha256));
        if (task.id != std::string{local_goose_dialogue_task_prefix} + identity
            || task.idempotency_key
                != std::string{local_goose_dialogue_task_prefix}
                    + "request-id:" + sha256_hex(out.request_id)) {
            out.detail = "local Goose dialogue Task identity differs";
            return out;
        }

        gaudere::work::ResourceLimits expected_limits;
        expected_limits.max_input_bytes = max_input_bytes;
        expected_limits.max_output_bytes = max_result_bytes;
        expected_limits.max_runtime = std::chrono::minutes{10};
        expected_limits.max_attempts = 2;
        if (!same_limits(task.limits, expected_limits)) {
            out.detail = "local Goose dialogue resource bounds differ";
            return out;
        }

        out.canonical_input = task.input;
        out.eligible = true;
        return out;
    } catch (const std::exception& error) {
        out.detail = error.what();
        return out;
    } catch (...) {
        out.detail = "local Goose dialogue inspection failed";
        return out;
    }
}

std::string make_local_goose_dialogue_response(
    const LocalGooseDialogueInspection& dialogue,
    const std::string& response)
{
    if (!dialogue.eligible) {
        throw std::invalid_argument(
            "cannot create response for invalid local Goose dialogue");
    }
    if (response.empty() || response.size() > max_response_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue response must be 1..16384 bytes");
    }

    const Json result{
        {"model_sha256", dialogue.model_sha256},
        {"request_id", dialogue.request_id},
        {"response", response},
        {"schema", local_goose_dialogue_response_schema}
    };
    const auto encoded = result.dump();
    if (encoded.size() > max_result_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue canonical response exceeds bound");
    }
    return encoded;
}

LocalGooseDialogueResponseInspection
inspect_local_goose_dialogue_response(
    const Task& task,
    const std::string& raw) noexcept
{
    LocalGooseDialogueResponseInspection out;
    try {
        const auto dialogue = inspect_local_goose_dialogue_task(task);
        if (!dialogue.eligible
            || raw.empty() || raw.size() > max_result_bytes) {
            out.detail =
                "local Goose dialogue response request/envelope is invalid";
            return out;
        }

        const auto parsed = Json::parse(raw);
        const std::set<std::string> expected{
            "model_sha256", "request_id", "response", "schema"};
        std::set<std::string> keys;
        if (!parsed.is_object()) {
            out.detail = "local Goose dialogue response is not an object";
            return out;
        }
        for (const auto& item : parsed.items()) keys.insert(item.key());
        if (keys != expected
            || parsed.value("schema", "")
                != local_goose_dialogue_response_schema
            || !parsed.at("request_id").is_string()
            || !parsed.at("response").is_string()
            || !parsed.at("model_sha256").is_string()
            || parsed.dump() != raw) {
            out.detail =
                "local Goose dialogue response JSON schema/canonical bytes differ";
            return out;
        }

        out.request_id = parsed.at("request_id").get<std::string>();
        out.response = parsed.at("response").get<std::string>();
        out.model_sha256 = parsed.at("model_sha256").get<std::string>();
        if (out.request_id != dialogue.request_id
            || out.model_sha256 != dialogue.model_sha256
            || out.response.empty()
            || out.response.size() > max_response_bytes) {
            out.detail =
                "local Goose dialogue response evidence differs from request";
            return out;
        }

        out.canonical_response = raw;
        out.eligible = true;
        return out;
    } catch (const std::exception& error) {
        out.detail = error.what();
        return out;
    } catch (...) {
        out.detail = "local Goose dialogue response inspection failed";
        return out;
    }
}

bool canonical_local_goose_dialogue_success(const Task& task) noexcept
{
    if (!inspect_local_goose_dialogue_task(task).eligible
        || task.status != TaskStatus::succeeded
        || task.attempts_started == 0
        || task.attempts_started > task.limits.max_attempts
        || !task.result
        || task.result->content_type
            != local_goose_dialogue_response_content_type
        || !task.result->failure_code.empty()
        || !task.result->failure_message.empty()) {
        return false;
    }
    return inspect_local_goose_dialogue_response(
        task, task.result->output).eligible;
}

} // namespace gaudere_agent
