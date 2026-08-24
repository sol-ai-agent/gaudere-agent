#include "ResumeContextSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
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
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0u) != 0x80u) return false;
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

bool safe_ref(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_provenance_ref_bytes
        || !valid_utf8(value)) return false;
    for (const unsigned char c : value) {
        if (c < 0x20u || c == 0x7fu) return false;
    }
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool allowed_kind(const std::string& value) noexcept
{
    return value == "github-blob" || value == "drive-revision"
        || value == "b10-proof" || value == "runtime-snapshot";
}

bool allowed_content_type(const std::string& value) noexcept
{
    return value == "text/plain; charset=utf-8"
        || value == "text/markdown; charset=utf-8";
}

Json parse_strict(const std::string& input)
{
    bool duplicate = false;
    std::vector<std::set<std::string>> stack;
    const auto callback = [&](int, const Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::object_start) stack.emplace_back();
        else if (event == Json::parse_event_t::key && !stack.empty()) {
            duplicate = duplicate || !stack.back().insert(parsed.get<std::string>()).second;
        } else if (event == Json::parse_event_t::object_end && !stack.empty()) {
            stack.pop_back();
        }
        return true;
    };
    Json parsed = Json::parse(input, callback);
    if (duplicate) throw std::invalid_argument("duplicate JSON key");
    return parsed;
}

bool exact_keys(const Json& object, const std::set<std::string>& keys)
{
    if (!object.is_object() || object.size() != keys.size()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (keys.find(it.key()) == keys.end()) return false;
    }
    return true;
}

std::uint32_t rotr(const std::uint32_t value, const unsigned count) noexcept
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
            const auto s0 = rotr(w[i-15],7u)^rotr(w[i-15],18u)^(w[i-15]>>3u);
            const auto s1 = rotr(w[i-2],17u)^rotr(w[i-2],19u)^(w[i-2]>>10u);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto a=hash[0], b=hash[1], c=hash[2], d=hash[3];
        auto e=hash[4], f=hash[5], g=hash[6], h=hash[7];
        for (std::size_t i=0;i<64;++i) {
            const auto s1=rotr(e,6u)^rotr(e,11u)^rotr(e,25u);
            const auto ch=(e&f)^((~e)&g);
            const auto t1=h+s1+ch+k[i]+w[i];
            const auto s0=rotr(a,2u)^rotr(a,13u)^rotr(a,22u);
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

} // namespace

ResumeContextSnapshotInspection inspect_resume_context_snapshot(
    const gaudere::work::Task& task) noexcept
{
    try {
        if (task.kind != resume_context_snapshot_task_kind
            || task.input_content_type != resume_context_snapshot_content_type
            || task.idempotency_key != task.id
            || task.input.empty() || task.input.size() > max_capsule_bytes
            || task.limits.max_input_bytes != max_capsule_bytes
            || task.limits.max_output_bytes != max_capsule_bytes
            || task.limits.max_runtime != std::chrono::seconds{2}
            || task.limits.max_attempts != 2
            || task.status != TaskStatus::succeeded || !task.result
            || task.result->content_type != resume_context_snapshot_content_type
            || task.result->output != task.input
            || !task.result->failure_code.empty()
            || !task.result->failure_message.empty()) {
            return {false, 0, {}, "snapshot Task/result shape is not canonical"};
        }
        const auto expected_id = std::string{resume_context_snapshot_task_prefix}
            + sha256_hex(task.input);
        if (task.id != expected_id) {
            return {false, 0, {}, "snapshot Task id does not match canonical content hash"};
        }

        const auto capsule = parse_strict(task.input);
        static const std::set<std::string> root_keys = {
            "schema", "captured_at_ms", "content_type", "content", "provenance"
        };
        if (!exact_keys(capsule, root_keys)
            || !capsule.at("schema").is_string()
            || capsule.at("schema").get<std::string>() != resume_context_snapshot_schema
            || !(capsule.at("captured_at_ms").is_number_integer()
                 || capsule.at("captured_at_ms").is_number_unsigned())
            || !capsule.at("content_type").is_string()
            || !capsule.at("content").is_string()
            || !capsule.at("provenance").is_array()) {
            return {false, 0, {}, "snapshot capsule schema is invalid"};
        }
        const auto captured = capsule.at("captured_at_ms").get<std::int64_t>();
        const auto content_type = capsule.at("content_type").get<std::string>();
        const auto content = capsule.at("content").get<std::string>();
        const auto& provenance = capsule.at("provenance");
        if (captured < 0 || !allowed_content_type(content_type)
            || content.empty() || content.size() > max_content_bytes
            || !valid_utf8(content)
            || provenance.empty() || provenance.size() > max_provenance_entries) {
            return {false, 0, {}, "snapshot capsule values are invalid"};
        }
        static const std::set<std::string> provenance_keys = {"kind", "ref", "sha256"};
        for (const auto& entry : provenance) {
            if (!exact_keys(entry, provenance_keys)
                || !entry.at("kind").is_string()
                || !entry.at("ref").is_string()
                || !entry.at("sha256").is_string()
                || !allowed_kind(entry.at("kind").get<std::string>())
                || !safe_ref(entry.at("ref").get<std::string>())
                || !lowercase_sha256(entry.at("sha256").get<std::string>())) {
                return {false, 0, {}, "snapshot provenance is invalid"};
            }
        }
        if (capsule.dump() != task.input) {
            return {false, 0, {}, "snapshot capsule is not canonical JSON"};
        }
        return {true, captured, task.input, {}};
    } catch (...) {
        return {false, 0, {}, "snapshot inspection failed closed"};
    }
}

} // namespace gaudere_agent
