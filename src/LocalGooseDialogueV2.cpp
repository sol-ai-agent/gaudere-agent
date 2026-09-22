#include "LocalGooseDialogueV2.hpp"

#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
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
constexpr std::size_t max_input_bytes = 12 * 1024;
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

Task make_task(const std::string& request_id,
               const std::string& message,
               const std::string& model_sha256,
               const std::uint64_t turn_index,
               const std::string& root_task_id,
               const std::string& predecessor_task_id,
               const std::string& predecessor_result_sha256)
{
    if (!safe_request_id(request_id)) {
        throw std::invalid_argument(
            "local Goose dialogue v2 request id is invalid");
    }
    if (message.empty() || message.size() > max_message_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue v2 message must be 1..4096 bytes");
    }
    if (!lowercase_sha256(model_sha256)) {
        throw std::invalid_argument(
            "local Goose dialogue v2 model sha256 is invalid");
    }

    const bool root = turn_index == 0;
    if (root) {
        if (!root_task_id.empty()
            || !predecessor_task_id.empty()
            || !predecessor_result_sha256.empty()) {
            throw std::invalid_argument(
                "local Goose dialogue v2 root lineage fields must be empty");
        }
    } else {
        if (!prefixed_sha256(root_task_id, local_goose_dialogue_v2_task_prefix)
            || !prefixed_sha256(
                predecessor_task_id, local_goose_dialogue_v2_task_prefix)
            || !lowercase_sha256(predecessor_result_sha256)) {
            throw std::invalid_argument(
                "local Goose dialogue v2 successor lineage is invalid");
        }
    }

    const Json input{
        {"message", message},
        {"model_sha256", model_sha256},
        {"predecessor_result_sha256", predecessor_result_sha256},
        {"predecessor_task_id", predecessor_task_id},
        {"request_id", request_id},
        {"root_task_id", root_task_id},
        {"schema", local_goose_dialogue_v2_schema},
        {"turn_index", turn_index}
    };
    const auto encoded = input.dump();
    if (encoded.size() > max_input_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue v2 canonical input exceeds bound");
    }

    Task task;
    task.id = std::string{local_goose_dialogue_v2_task_prefix}
        + sha256_hex(encoded);
    task.idempotency_key = std::string{local_goose_dialogue_v2_task_prefix}
        + "request-id:" + sha256_hex(request_id);
    task.kind = local_goose_dialogue_v2_task_kind;
    task.input_content_type = local_goose_dialogue_v2_content_type;
    task.input = encoded;
    task.limits.max_input_bytes = max_input_bytes;
    task.limits.max_output_bytes = max_result_bytes;
    task.limits.max_runtime = std::chrono::minutes{10};
    task.limits.max_attempts = 2;
    return task;
}

} // namespace

Task make_local_goose_dialogue_v2_root_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256)
{
    return make_task(
        request_id, message, model_sha256, 0, {}, {}, {});
}

Task make_local_goose_dialogue_v2_successor_task(
    const std::string& request_id,
    const std::string& message,
    const std::string& model_sha256,
    const Task& predecessor)
{
    if (!canonical_local_goose_dialogue_v2_success(predecessor)
        || !predecessor.result) {
        throw std::invalid_argument(
            "local Goose dialogue v2 predecessor is not canonical success");
    }

    const auto predecessor_dialogue =
        inspect_local_goose_dialogue_v2_task(predecessor);
    const auto predecessor_response =
        inspect_local_goose_dialogue_v2_response(
            predecessor, predecessor.result->output);
    if (!predecessor_dialogue.eligible
        || !predecessor_response.eligible
        || predecessor_dialogue.model_sha256 != model_sha256
        || predecessor_response.model_sha256 != model_sha256) {
        throw std::invalid_argument(
            "local Goose dialogue v2 predecessor model differs");
    }
    if (predecessor_dialogue.turn_index
        == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument(
            "local Goose dialogue v2 turn index overflow");
    }

    return make_task(
        request_id,
        message,
        model_sha256,
        predecessor_dialogue.turn_index + 1,
        predecessor_response.root_task_id,
        predecessor.id,
        sha256_hex(predecessor.result->output));
}

LocalGooseDialogueV2Inspection
inspect_local_goose_dialogue_v2_task(const Task& task) noexcept
{
    LocalGooseDialogueV2Inspection out;
    try {
        if (!prefixed_sha256(task.id, local_goose_dialogue_v2_task_prefix)
            || task.kind != local_goose_dialogue_v2_task_kind
            || task.input_content_type != local_goose_dialogue_v2_content_type
            || task.input.empty() || task.input.size() > max_input_bytes) {
            out.detail = "local Goose dialogue v2 Task envelope is invalid";
            return out;
        }

        const auto parsed = Json::parse(task.input);
        const std::set<std::string> expected{
            "message",
            "model_sha256",
            "predecessor_result_sha256",
            "predecessor_task_id",
            "request_id",
            "root_task_id",
            "schema",
            "turn_index"
        };
        std::set<std::string> keys;
        if (!parsed.is_object()) {
            out.detail = "local Goose dialogue v2 input is not an object";
            return out;
        }
        for (const auto& item : parsed.items()) keys.insert(item.key());
        if (keys != expected
            || parsed.value("schema", "") != local_goose_dialogue_v2_schema
            || !parsed.at("message").is_string()
            || !parsed.at("model_sha256").is_string()
            || !parsed.at("predecessor_result_sha256").is_string()
            || !parsed.at("predecessor_task_id").is_string()
            || !parsed.at("request_id").is_string()
            || !parsed.at("root_task_id").is_string()
            || !parsed.at("turn_index").is_number_unsigned()
            || parsed.dump() != task.input) {
            out.detail =
                "local Goose dialogue v2 JSON schema/canonical bytes differ";
            return out;
        }

        out.task_id = task.id;
        out.request_id = parsed.at("request_id").get<std::string>();
        out.message = parsed.at("message").get<std::string>();
        out.model_sha256 = parsed.at("model_sha256").get<std::string>();
        out.turn_index = parsed.at("turn_index").get<std::uint64_t>();
        out.root_task_id = parsed.at("root_task_id").get<std::string>();
        out.predecessor_task_id =
            parsed.at("predecessor_task_id").get<std::string>();
        out.predecessor_result_sha256 =
            parsed.at("predecessor_result_sha256").get<std::string>();

        if (!safe_request_id(out.request_id)
            || out.message.empty() || out.message.size() > max_message_bytes
            || !lowercase_sha256(out.model_sha256)) {
            out.detail = "local Goose dialogue v2 fields are invalid";
            return out;
        }

        if (out.turn_index == 0) {
            if (!out.root_task_id.empty()
                || !out.predecessor_task_id.empty()
                || !out.predecessor_result_sha256.empty()) {
                out.detail =
                    "local Goose dialogue v2 root lineage fields differ";
                return out;
            }
        } else {
            if (!prefixed_sha256(
                    out.root_task_id, local_goose_dialogue_v2_task_prefix)
                || !prefixed_sha256(
                    out.predecessor_task_id,
                    local_goose_dialogue_v2_task_prefix)
                || !lowercase_sha256(out.predecessor_result_sha256)
                || out.root_task_id == task.id
                || out.predecessor_task_id == task.id) {
                out.detail =
                    "local Goose dialogue v2 successor lineage fields differ";
                return out;
            }
        }

        out.message_sha256 = sha256_hex(out.message);
        if (task.id != std::string{local_goose_dialogue_v2_task_prefix}
                + sha256_hex(task.input)
            || task.idempotency_key
                != std::string{local_goose_dialogue_v2_task_prefix}
                    + "request-id:" + sha256_hex(out.request_id)) {
            out.detail = "local Goose dialogue v2 Task identity differs";
            return out;
        }

        gaudere::work::ResourceLimits expected_limits;
        expected_limits.max_input_bytes = max_input_bytes;
        expected_limits.max_output_bytes = max_result_bytes;
        expected_limits.max_runtime = std::chrono::minutes{10};
        expected_limits.max_attempts = 2;
        if (!same_limits(task.limits, expected_limits)) {
            out.detail = "local Goose dialogue v2 resource bounds differ";
            return out;
        }

        out.canonical_input = task.input;
        out.eligible = true;
        return out;
    } catch (const std::exception& error) {
        out.detail = error.what();
        return out;
    } catch (...) {
        out.detail = "local Goose dialogue v2 inspection failed";
        return out;
    }
}

std::string make_local_goose_dialogue_v2_response(
    const LocalGooseDialogueV2Inspection& dialogue,
    const std::string& response)
{
    if (!dialogue.eligible) {
        throw std::invalid_argument(
            "cannot create response for invalid local Goose dialogue v2");
    }
    if (response.empty() || response.size() > max_response_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue v2 response must be 1..16384 bytes");
    }

    const auto root_task_id =
        dialogue.turn_index == 0 ? dialogue.task_id : dialogue.root_task_id;
    const Json result{
        {"model_sha256", dialogue.model_sha256},
        {"predecessor_result_sha256", dialogue.predecessor_result_sha256},
        {"predecessor_task_id", dialogue.predecessor_task_id},
        {"request_id", dialogue.request_id},
        {"response", response},
        {"root_task_id", root_task_id},
        {"schema", local_goose_dialogue_v2_response_schema},
        {"turn_index", dialogue.turn_index}
    };
    const auto encoded = result.dump();
    if (encoded.size() > max_result_bytes) {
        throw std::invalid_argument(
            "local Goose dialogue v2 canonical response exceeds bound");
    }
    return encoded;
}

LocalGooseDialogueV2ResponseInspection
inspect_local_goose_dialogue_v2_response(
    const Task& task,
    const std::string& raw) noexcept
{
    LocalGooseDialogueV2ResponseInspection out;
    try {
        const auto dialogue = inspect_local_goose_dialogue_v2_task(task);
        if (!dialogue.eligible
            || raw.empty() || raw.size() > max_result_bytes) {
            out.detail =
                "local Goose dialogue v2 response request/envelope is invalid";
            return out;
        }

        const auto parsed = Json::parse(raw);
        const std::set<std::string> expected{
            "model_sha256",
            "predecessor_result_sha256",
            "predecessor_task_id",
            "request_id",
            "response",
            "root_task_id",
            "schema",
            "turn_index"
        };
        std::set<std::string> keys;
        if (!parsed.is_object()) {
            out.detail =
                "local Goose dialogue v2 response is not an object";
            return out;
        }
        for (const auto& item : parsed.items()) keys.insert(item.key());
        if (keys != expected
            || parsed.value("schema", "")
                != local_goose_dialogue_v2_response_schema
            || !parsed.at("model_sha256").is_string()
            || !parsed.at("predecessor_result_sha256").is_string()
            || !parsed.at("predecessor_task_id").is_string()
            || !parsed.at("request_id").is_string()
            || !parsed.at("response").is_string()
            || !parsed.at("root_task_id").is_string()
            || !parsed.at("turn_index").is_number_unsigned()
            || parsed.dump() != raw) {
            out.detail =
                "local Goose dialogue v2 response JSON/canonical bytes differ";
            return out;
        }

        out.request_id = parsed.at("request_id").get<std::string>();
        out.root_task_id = parsed.at("root_task_id").get<std::string>();
        out.turn_index = parsed.at("turn_index").get<std::uint64_t>();
        out.predecessor_task_id =
            parsed.at("predecessor_task_id").get<std::string>();
        out.predecessor_result_sha256 =
            parsed.at("predecessor_result_sha256").get<std::string>();
        out.response = parsed.at("response").get<std::string>();
        out.model_sha256 = parsed.at("model_sha256").get<std::string>();

        const auto expected_root =
            dialogue.turn_index == 0 ? task.id : dialogue.root_task_id;
        if (out.request_id != dialogue.request_id
            || out.root_task_id != expected_root
            || out.turn_index != dialogue.turn_index
            || out.predecessor_task_id != dialogue.predecessor_task_id
            || out.predecessor_result_sha256
                != dialogue.predecessor_result_sha256
            || out.model_sha256 != dialogue.model_sha256
            || out.response.empty()
            || out.response.size() > max_response_bytes) {
            out.detail =
                "local Goose dialogue v2 response evidence differs from request";
            return out;
        }

        out.canonical_response = raw;
        out.eligible = true;
        return out;
    } catch (const std::exception& error) {
        out.detail = error.what();
        return out;
    } catch (...) {
        out.detail = "local Goose dialogue v2 response inspection failed";
        return out;
    }
}

bool canonical_local_goose_dialogue_v2_success(const Task& task) noexcept
{
    if (!inspect_local_goose_dialogue_v2_task(task).eligible
        || task.status != TaskStatus::succeeded
        || task.attempts_started == 0
        || task.attempts_started > task.limits.max_attempts
        || !task.result
        || task.result->content_type
            != local_goose_dialogue_v2_response_content_type
        || !task.result->failure_code.empty()
        || !task.result->failure_message.empty()) {
        return false;
    }
    return inspect_local_goose_dialogue_v2_response(
        task, task.result->output).eligible;
}

} // namespace gaudere_agent
