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
        if (first <= 0x7fu) {
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
    bool duplicate = false;
    std::vector<std::set<std::string>> stack;
    const auto callback = [&](int, const Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) {
            stack.emplace_back();
        } else if (event == Json::parse_event_t::key && !stack.empty()) {
            duplicate = duplicate
                || !stack.back().insert(parsed.get<std::string>()).second;
        } else if (event == Json::parse_event_t::object_end && !stack.empty()) {
            stack.pop_back();
        }
        return true;
    };
    Json parsed = Json::parse(input, callback);
    if (duplicate) {
        throw std::invalid_argument("context JSON contains a duplicate key");
    }
    return parsed;
}

void validate_request(const Json& request)
{
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
        throw std::invalid_argument("context request does not match snapshot schema");
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
        if (!allowed_provenance_kind(entry.at("kind").get<std::string>())) {
            throw std::invalid_argument("context provenance kind is not allowed");
        }
        if (!safe_ref_text(entry.at("ref").get<std::string>())) {
            throw std::invalid_argument("context provenance ref is invalid");
        }
        if (!lowercase_sha256(entry.at("sha256").get<std::string>())) {
            throw std::invalid_argument("context provenance sha256 is invalid");
        }
    }
}

Json parse_request(const std::string& request_json)
{
    try {
        auto request = parse_without_duplicate_keys(request_json);
        validate_request(request);
        return request;
    } catch (const std::invalid_argument&) {
        throw;
    } catch (...) {
        throw std::invalid_argument("context request is not valid JSON");
    }
}

std::int64_t milliseconds(const gaudere::work::TimePoint value)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count();
}

std::string canonical_capsule(const Json& request,
                              const gaudere::work::TimePoint captured_at)
{
    const auto captured = milliseconds(captured_at);
    if (captured < 0) {
        throw std::invalid_argument("context capture time precedes Unix epoch");
    }
    Json capsule = {
        {"schema", resume_context_snapshot_schema},
        {"captured_at_ms", captured},
        {"content_type", request.at("content_type")},
        {"content", request.at("content")},
        {"provenance", request.at("provenance")}
    };
    const auto canonical = capsule.dump();
    if (canonical.size() > max_capsule_bytes) {
        throw std::invalid_argument("canonical context capsule exceeds 24576 bytes");
    }
    return canonical;
}

Json request_from_stored_capsule(const std::string& input)
{
    Json capsule;
    try {
        capsule = parse_without_duplicate_keys(input);
    } catch (const std::invalid_argument&) {
        throw;
    } catch (...) {
        throw std::invalid_argument("stored context capsule is not valid JSON");
    }
    static const std::set<std::string> keys = {
        "schema", "captured_at_ms", "content_type", "content", "provenance"
    };
    if (!only_keys(capsule, keys) || capsule.size() != keys.size()
        || !capsule.contains("captured_at_ms")
        || !(capsule.at("captured_at_ms").is_number_integer()
             || capsule.at("captured_at_ms").is_number_unsigned())) {
        throw std::invalid_argument("stored context capsule schema is invalid");
    }
    const auto captured = capsule.at("captured_at_ms").get<std::int64_t>();
    if (captured < 0) {
        throw std::invalid_argument("stored context capture time is invalid");
    }
    Json request = {
        {"schema", capsule.value("schema", "")},
        {"content_type", capsule.value("content_type", "")},
        {"content", capsule.value("content", "")},
        {"provenance", capsule.contains("provenance")
             ? capsule.at("provenance") : Json::array()}
    };
    validate_request(request);
    const auto expected = canonical_capsule(
        request, gaudere::work::TimePoint{std::chrono::milliseconds{captured}});
    if (expected != input) {
        throw std::invalid_argument("stored context capsule is not canonical");
    }
    return request;
}

std::string request_signature(const Json& request)
{
    return canonical_capsule(request, gaudere::work::TimePoint{});
}

std::uint32_t rotate_right(const std::uint32_t value, const unsigned count) noexcept
{
    return (value >> count) | (value << (32u - count));
}

std::string sha256_hex(const std::string& input)
{
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    std::array<std::uint32_t, 8> hash = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    std::vector<unsigned char> bytes(input.begin(), input.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8u;
    bytes.push_back(0x80u);
    while ((bytes.size() % 64u) != 56u) bytes.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.push_back(static_cast<unsigned char>((bit_length >> shift) & 0xffu));
    for (std::size_t base = 0; base < bytes.size(); base += 64u) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto o = base + i * 4u;
            w[i] = (static_cast<std::uint32_t>(bytes[o]) << 24u)
                | (static_cast<std::uint32_t>(bytes[o+1]) << 16u)
                | (static_cast<std::uint32_t>(bytes[o+2]) << 8u)
                | static_cast<std::uint32_t>(bytes[o+3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotate_right(w[i-15],7u)^rotate_right(w[i-15],18u)^(w[i-15]>>3u);
            const auto s1 = rotate_right(w[i-2],17u)^rotate_right(w[i-2],19u)^(w[i-2]>>10u);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto a=hash[0], b=hash[1], c=hash[2], d=hash[3];
        auto e=hash[4], f=hash[5], g=hash[6], h=hash[7];
        for (std::size_t i=0;i<64;++i) {
            const auto s1=rotate_right(e,6u)^rotate_right(e,11u)^rotate_right(e,25u);
            const auto ch=(e&f)^((~e)&g);
            const auto t1=h+s1+ch+k[i]+w[i];
            const auto s0=rotate_right(a,2u)^rotate_right(a,13u)^rotate_right(a,22u);
            const auto maj=(a&b)^(a&c)^(b&c);
            const auto t2=s0+maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        hash[0]+=a; hash[1]+=b; hash[2]+=c; hash[3]+=d;
        hash[4]+=e; hash[5]+=f; hash[6]+=g; hash[7]+=h;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : hash) out << std::setw(8) << value;
    return out.str();
}

bool same_limits(const gaudere::work::ResourceLimits& a,
                 const gaudere::work::ResourceLimits& b) noexcept
{
    return a.max_input_bytes == b.max_input_bytes
        && a.max_output_bytes == b.max_output_bytes
        && a.max_runtime == b.max_runtime && a.max_attempts == b.max_attempts;
}

bool same_definition(const Task& a, const Task& b) noexcept
{
    return a.id == b.id && a.idempotency_key == b.idempotency_key
        && a.kind == b.kind && a.input_content_type == b.input_content_type
        && a.input == b.input && same_limits(a.limits, b.limits);
}

Task make_task(const std::string& canonical)
{
    Task task;
    task.id = std::string{resume_context_snapshot_task_prefix} + sha256_hex(canonical);
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

bool canonical_task_input(const Task& task) noexcept
{
    try {
        if (task.kind != resume_context_snapshot_task_kind
            || task.input_content_type != resume_context_snapshot_content_type
            || task.input.empty() || task.input.size() > max_capsule_bytes) {
            return false;
        }
        static_cast<void>(request_from_stored_capsule(task.input));
        return make_task(task.input).id == task.id
            && task.idempotency_key == task.id;
    } catch (...) {
        return false;
    }
}

class SnapshotIdentityHandler final : public TaskHandler {
public:
    HandlerResult execute(const TaskContext& context) override
    {
        if (context.cancellation_requested())
            return HandlerResult{HandlerOutcome::cancelled, {}, {}, {}, {}};
        if (!canonical_task_input(context.task)) {
            return HandlerResult{HandlerOutcome::failed, {}, {},
                                 "invalid_resume_context_snapshot",
                                 "snapshot Task is not canonical"};
        }
        return HandlerResult{HandlerOutcome::succeeded,
                             resume_context_snapshot_content_type,
                             context.task.input, {}, {}};
    }
};

bool canonical_success(const Task& task) noexcept
{
    return task.status == TaskStatus::succeeded && task.result
        && canonical_task_input(task)
        && task.result->content_type == resume_context_snapshot_content_type
        && task.result->output == task.input
        && task.result->failure_code.empty() && task.result->failure_message.empty();
}

ResumeContextSnapshotRecord complete_task(gaudere::work::Runtime& runtime,
                                          gaudere::work::TaskStore& store,
                                          const Task& task,
                                          const ResumeContextSnapshotRecordResult success_result)
{
    SnapshotIdentityHandler handler;
    TaskExecutor executor(runtime, store);
    if (executor.execute(task.id, "resume-context-recorder", handler)
        != ExecuteResult::completed) {
        return {ResumeContextSnapshotRecordResult::unavailable, store.find(task.id),
                "snapshot Task is not currently startable"};
    }
    const auto completed = store.find(task.id);
    if (!completed || !canonical_success(*completed)) {
        return {ResumeContextSnapshotRecordResult::conflict, completed,
                "snapshot Task did not complete canonically"};
    }
    return {success_result, completed, {}};
}

} // namespace

ResumeContextSnapshotRecorder::ResumeContextSnapshotRecorder(
    gaudere::work::TaskStore& task_store,
    gaudere::work::Runtime& work_runtime,
    Now now)
    : task_store_(task_store), work_runtime_(work_runtime), now_(std::move(now))
{
    if (!now_) throw std::invalid_argument("resume context snapshot clock is required");
}

ResumeContextSnapshotRecord ResumeContextSnapshotRecorder::record(
    const std::string& request_json)
{
    if (work_runtime_.state() != gaudere::work::RuntimeState::running) {
        return {ResumeContextSnapshotRecordResult::unavailable, {},
                "work runtime is not running"};
    }

    Json request;
    std::string signature;
    try {
        request = parse_request(request_json);
        signature = request_signature(request);
    } catch (const std::exception& error) {
        return {ResumeContextSnapshotRecordResult::invalid, {}, error.what()};
    }

    // Recover provider-free snapshot Tasks left pending by a previous recorder crash.
    // If one is the same logical request, return that recovered durable snapshot
    // instead of assigning a new capture time and silently creating another one.
    while (const auto pending = task_store_.find_pending_for(
               {resume_context_snapshot_task_kind})) {
        bool same_request = false;
        try {
            same_request = request_signature(
                request_from_stored_capsule(pending->input)) == signature;
        } catch (...) {
            const auto closed = complete_task(work_runtime_, task_store_, *pending,
                                              ResumeContextSnapshotRecordResult::duplicate);
            return {ResumeContextSnapshotRecordResult::conflict, closed.task,
                    "pending snapshot Task is corrupt"};
        }
        const auto closed = complete_task(work_runtime_, task_store_, *pending,
                                          ResumeContextSnapshotRecordResult::duplicate);
        if (closed.result != ResumeContextSnapshotRecordResult::duplicate) {
            return closed;
        }
        if (same_request) return closed;
    }

    std::string canonical;
    try {
        canonical = canonical_capsule(request, now_());
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
            if (!canonical_success(*existing)) {
                return {ResumeContextSnapshotRecordResult::conflict, existing,
                        "terminal snapshot Task lacks canonical successful result"};
            }
            return {ResumeContextSnapshotRecordResult::duplicate, existing, {}};
        }
        return complete_task(work_runtime_, task_store_, *existing,
                             ResumeContextSnapshotRecordResult::duplicate);
    }

    switch (work_runtime_.submit(expected)) {
    case gaudere::work::SubmitResult::accepted:
        break;
    case gaudere::work::SubmitResult::duplicate:
        return {ResumeContextSnapshotRecordResult::conflict, {},
                "snapshot submission became duplicate without durable match"};
    case gaudere::work::SubmitResult::invalid:
        return {ResumeContextSnapshotRecordResult::conflict, {},
                "canonical snapshot Task was rejected as invalid"};
    case gaudere::work::SubmitResult::unavailable:
        return {ResumeContextSnapshotRecordResult::unavailable, {},
                "work runtime rejected snapshot submission"};
    }
    return complete_task(work_runtime_, task_store_, expected,
                         ResumeContextSnapshotRecordResult::accepted);
}

} // namespace gaudere_agent
