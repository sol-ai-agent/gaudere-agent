#include "ResumeContextSnapshot.hpp"

#include "TaskExecutor.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
using Task = gaudere::work::Task;
using TaskStatus = gaudere::work::TaskStatus;

constexpr std::size_t max_content_bytes = 16 * 1024;
constexpr std::size_t max_capsule_bytes = 24 * 1024;
constexpr std::size_t max_provenance_entries = 8;
constexpr std::size_t max_provenance_ref_bytes = 1024;

bool valid_utf8(const std::string& value) noexcept
{
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if ((first & 0xe0u) == 0xc0u) {
            length = 2;
            codepoint = first & 0x1fu;
        } else if ((first & 0xf0u) == 0xe0u) {
            length = 3;
            codepoint = first & 0x0fu;
        } else if ((first & 0xf8u) == 0xf0u) {
            length = 4;
            codepoint = first & 0x07u;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6u) | (next & 0x3fu);
        }
        if ((length == 2 && codepoint < 0x80u)
            || (length == 3 && codepoint < 0x800u)
            || (length == 4 && codepoint < 0x10000u)
            || codepoint > 0x10ffffu
            || (codepoint >= 0xd800u && codepoint <= 0xdfffu)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool safe_ref_text(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_provenance_ref_bytes
        || !valid_utf8(value)) {
        return false;
    }
    for (const unsigned char character : value) {
        if (character < 0x20u || character == 0x7fu) {
            return false;
        }
    }
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool allowed_provenance_kind(const std::string& value) noexcept
{
    return value == "github-blob" || value == "drive-revision"
        || value == "b10-proof" || value == "runtime-snapshot";
}

bool allowed_content_type(const std::string& value) noexcept
{
    return value == "text/plain; charset=utf-8"
        || value == "text/markdown; charset=utf-8";
}

bool only_keys(const Json& object, const std::set<std::string>& allowed)
{
    if (!object.is_object()) {
        return false;
    }
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (allowed.find(item.key()) == allowed.end()) {
            return false;
        }
    }
    return true;
}

Json parse_without_duplicate_keys(const std::string& input)
{
    bool duplicate_key = false;
    std::vector<std::set<std::string>> object_keys;
    const auto callback = [&](int, const Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == Json::parse_event_t::key && !object_keys.empty()) {
            const auto inserted = object_keys.back().insert(parsed.get<std::string>());
            duplicate_key = duplicate_key || !inserted.second;
        } else if (event == Json::parse_event_t::object_end && !object_keys.empty()) {
            object_keys.pop_back();
        }
        return true;
    };

    Json parsed = Json::parse(input, callback);
    if (duplicate_key) {
        throw std::invalid_argument("context request contains a duplicate JSON key");
    }
    return parsed;
}

std::uint32_t rotate_right(const std::uint32_t value, const unsigned count) noexcept
{
    return (value >> count) | (value << (32u - count));
}

std::string sha256_hex(const std::string& input)
{
    static constexpr std::array<std::uint32_t, 64> constants = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    std::array<std::uint32_t, 8> hash = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    std::vector<unsigned char> bytes(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8u;
    bytes.push_back(0x80u);
    while ((bytes.size() % 64u) != 56u) {
        bytes.push_back(0u);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<unsigned char>((bit_length >> shift) & 0xffu));
    }

    for (std::size_t base = 0; base < bytes.size(); base += 64u) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = base + index * 4u;
            words[index] = (static_cast<std::uint32_t>(bytes[offset]) << 24u)
                | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16u)
                | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8u)
                | static_cast<std::uint32_t>(bytes[offset + 3]);
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const auto s0 = rotate_right(words[index - 15], 7u)
                ^ rotate_right(words[index - 15], 18u)
                ^ (words[index - 15] >> 3u);
            const auto s1 = rotate_right(words[index - 2], 17u)
                ^ rotate_right(words[index - 2], 19u)
                ^ (words[index - 2] >> 10u);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }

        std::uint32_t a = hash[0];
        std::uint32_t b = hash[1];
        std::uint32_t c = hash[2];
        std::uint32_t d = hash[3];
        std::uint32_t e = hash[4];
        std::uint32_t f = hash[5];
        std::uint32_t g = hash[6];
        std::uint32_t h = hash[7];
        for (std::size_t index = 0; index < 64; ++index) {
            const auto s1 = rotate_right(e, 6u) ^ rotate_right(e, 11u)
                ^ rotate_right(e, 25u);
            const auto choose = (e & f) ^ ((~e) & g);
            const auto temp1 = h + s1 + choose + constants[index] + words[index];
            const auto s0 = rotate_right(a, 2u) ^ rotate_right(a, 13u)
                ^ rotate_right(a, 22u);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : hash) {
        output << std::setw(8) << value;
    }
    return output.str();
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string canonical_capsule(const std::string& request_json,
                              const gaudere::work::TimePoint captured_at)
{
    Json request;
    try {
        request = parse_without_duplicate_keys(request_json);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (...) {
        throw std::invalid_argument("context request is not valid JSON");
    }

    static const std::set<std::string> request_keys = {
        "schema", "content_type", "content", "provenance"
    };
    if (!only_keys(request, request_keys) || request.size() != request_keys.size()
        || !request.contains("schema") || !request.at("schema").is_string()
        || request.at("schema").get<std::string>() != resume_context_snapshot_schema
        || !request.contains("content_type")
        || !request.at("content_type").is_string()
        || !request.contains("content") || !request.at("content").is_string()
        || !request.contains("provenance") || !request.at("provenance").is_array()) {
        throw std::invalid_argument("context request does not match the snapshot schema");
    }

    const auto content_type = request.at("content_type").get<std::string>();
    const auto content = request.at("content").get<std::string>();
    const auto& provenance = request.at("provenance");
    if (!allowed_content_type(content_type)) {
        throw std::invalid_argument("context content type is not allowed");
    }
    if (content.empty() || content.size() > max_content_bytes || !valid_utf8(content)) {
        throw std::invalid_argument("context content must be 1..16384 valid UTF-8 bytes");
    }
    if (provenance.empty() || provenance.size() > max_provenance_entries) {
        throw std::invalid_argument("context provenance must contain 1..8 entries");
    }

    static const std::set<std::string> provenance_keys = {"kind", "ref", "sha256"};
    for (const auto& entry : provenance) {
        if (!only_keys(entry, provenance_keys) || entry.size() != provenance_keys.size()
            || !entry.contains("kind") || !entry.at("kind").is_string()
            || !entry.contains("ref") || !entry.at("ref").is_string()
            || !entry.contains("sha256") || !entry.at("sha256").is_string()) {
            throw std::invalid_argument("context provenance entry is invalid");
        }
        const auto kind = entry.at("kind").get<std::string>();
        const auto ref = entry.at("ref").get<std::string>();
        const auto digest = entry.at("sha256").get<std::string>();
        if (!allowed_provenance_kind(kind)) {
            throw std::invalid_argument("context provenance kind is not allowed");
        }
        if (!safe_ref_text(ref)) {
            throw std::invalid_argument("context provenance ref is invalid");
        }
        if (!lowercase_sha256(digest)) {
            throw std::invalid_argument("context provenance sha256 is invalid");
        }
    }

    Json capsule = {
        {"schema", resume_context_snapshot_schema},
        {"captured_at_ms", milliseconds(captured_at)},
        {"content_type", content_type},
        {"content", content},
        {"provenance", provenance}
    };
    const auto canonical = capsule.dump();
    if (canonical.size() > max_capsule_bytes) {
        throw std::invalid_argument("canonical context capsule exceeds 24576 bytes");
    }
    return canonical;
}

bool same_limits(const gaudere::work::ResourceLimits& left,
                 const gaudere::work::ResourceLimits& right) noexcept
{
    return left.max_input_bytes == right.max_input_bytes
        && left.max_output_bytes == right.max_output_bytes
        && left.max_runtime == right.max_runtime
        && left.max_attempts == right.max_attempts;
}

bool same_definition(const Task& left, const Task& right) noexcept
{
    return left.id == right.id && left.idempotency_key == right.idempotency_key
        && left.kind == right.kind
        && left.input_content_type == right.input_content_type
        && left.input == right.input && same_limits(left.limits, right.limits);
}

Task make_task(const std::string& canonical)
{
    const auto digest = sha256_hex(canonical);
    Task task;
    task.id = std::string{resume_context_snapshot_task_prefix} + digest;
    task.idempotency_key = task.id;
    task.kind = resume_context_snapshot_task_kind;
    task.input_content_type = resume_context_snapshot_content_type;
    task.input = canonical;
    task.limits.max_input_bytes = max_capsule_bytes;
    task.limits.max_output_bytes = max_capsule_bytes;
    task.limits.max_runtime = std::chrono::seconds{2};
    task.limits.max_attempts = 2;
    return task;
}

class SnapshotIdentityHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        if (context.cancellation_requested()) {
            return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
        }
        if (context.task.kind != resume_context_snapshot_task_kind
            || context.task.input_content_type != resume_context_snapshot_content_type
            || context.task.input.empty()
            || context.task.input.size() > max_capsule_bytes) {
            return HandlerResult{HandlerOutcome::failed, {}, {},
                                 "invalid_resume_context_snapshot",
                                 "snapshot Task is not canonical"};
        }
        return HandlerResult{HandlerOutcome::succeeded,
                             resume_context_snapshot_content_type,
                             context.task.input, {}, {}};
    }
};

bool canonical_success(const Task& task, const std::string& canonical) noexcept
{
    return task.status == TaskStatus::succeeded && task.result
        && task.result->content_type == resume_context_snapshot_content_type
        && task.result->output == canonical
        && task.result->failure_code.empty()
        && task.result->failure_message.empty();
}

} // namespace

ResumeContextSnapshotRecorder::ResumeContextSnapshotRecorder(
    gaudere::work::TaskStore& task_store,
    gaudere::work::Runtime& work_runtime,
    Now now)
    : task_store_(task_store), work_runtime_(work_runtime), now_(std::move(now))
{
    if (!now_) {
        throw std::invalid_argument("resume context snapshot clock is required");
    }
}

ResumeContextSnapshotRecord ResumeContextSnapshotRecorder::record(
    const std::string& request_json)
{
    if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
        return {ResumeContextSnapshotRecordResult::unavailable, {},
                "work runtime is not running"};
    }

    std::string canonical;
    try {
        canonical = canonical_capsule(request_json, now_());
    } catch (const std::exception& error) {
        return {ResumeContextSnapshotRecordResult::invalid, {}, error.what()};
    }
    const auto expected = make_task(canonical);

    const auto by_id = task_store_.find(expected.id);
    const auto by_key = task_store_.find_by_idempotency_key(expected.idempotency_key);
    if (by_id || by_key) {
        if (by_id && by_key && by_id->id != by_key->id) {
            return {ResumeContextSnapshotRecordResult::conflict, {},
                    "snapshot Task id and idempotency key resolve differently"};
        }
        const auto existing = by_id ? by_id : by_key;
        if (!existing || !same_definition(*existing, expected)) {
            return {ResumeContextSnapshotRecordResult::conflict, existing,
                    "existing snapshot Task conflicts with canonical definition"};
        }
        if (gaudere::work::is_terminal(existing->status)) {
            if (!canonical_success(*existing, canonical)) {
                return {ResumeContextSnapshotRecordResult::conflict, existing,
                        "terminal snapshot Task lacks canonical successful result"};
            }
            return {ResumeContextSnapshotRecordResult::duplicate, existing, {}};
        }

        SnapshotIdentityHandler handler;
        TaskExecutor executor(work_runtime_, task_store_);
        if (executor.execute(existing->id, "resume-context-recorder", handler)
            != ExecuteResult::completed) {
            return {ResumeContextSnapshotRecordResult::unavailable, existing,
                    "existing snapshot Task is not currently startable"};
        }
        const auto completed = task_store_.find(existing->id);
        if (!completed || !canonical_success(*completed, canonical)) {
            return {ResumeContextSnapshotRecordResult::conflict, completed,
                    "snapshot Task did not complete canonically"};
        }
        return {ResumeContextSnapshotRecordResult::duplicate, completed, {}};
    }

    switch (work_runtime_.submit(expected)) {
    case gaudere::work::SubmitResult::accepted:
        break;
    case gaudere::work::SubmitResult::duplicate:
        return {ResumeContextSnapshotRecordResult::conflict, {},
                "snapshot submission became duplicate without prior durable match"};
    case gaudere::work::SubmitResult::invalid:
        return {ResumeContextSnapshotRecordResult::conflict, {},
                "canonical snapshot Task was rejected as invalid"};
    case gaudere::work::SubmitResult::unavailable:
        return {ResumeContextSnapshotRecordResult::unavailable, {},
                "work runtime rejected snapshot submission"};
    }

    SnapshotIdentityHandler handler;
    TaskExecutor executor(work_runtime_, task_store_);
    if (executor.execute(expected.id, "resume-context-recorder", handler)
        != ExecuteResult::completed) {
        return {ResumeContextSnapshotRecordResult::unavailable,
                task_store_.find(expected.id),
                "new snapshot Task could not complete locally"};
    }
    const auto completed = task_store_.find(expected.id);
    if (!completed || !canonical_success(*completed, canonical)) {
        return {ResumeContextSnapshotRecordResult::conflict, completed,
                "new snapshot Task did not persist canonical result"};
    }
    return {ResumeContextSnapshotRecordResult::accepted, completed, {}};
}

} // namespace gaudere_agent
