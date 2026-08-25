#include "ResumeContextSnapshot.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

constexpr std::size_t max_content_bytes = 16 * 1024;
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

bool safe_ref_text(const std::string& value) noexcept
{
    if (value.empty() || value.size() > max_provenance_ref_bytes
        || !valid_utf8(value)) return false;
    for (const unsigned char character : value) {
        if (character < 0x20u || character == 0x7fu) return false;
    }
    return true;
}

bool lowercase_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    for (const char character : value) {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) return false;
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

bool exact_keys(const Json& object, const std::set<std::string>& expected)
{
    if (!object.is_object() || object.size() != expected.size()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (expected.find(it.key()) == expected.end()) return false;
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
    auto parsed = Json::parse(input, callback);
    if (duplicate) throw std::invalid_argument("duplicate JSON key");
    return parsed;
}

} // namespace

ResumeContextSnapshotRequestInspection inspect_resume_context_snapshot_request(
    const std::string& request_json) noexcept
{
    try {
        const auto request = parse_without_duplicate_keys(request_json);
        static const std::set<std::string> request_keys = {
            "schema", "content_type", "content", "provenance"
        };
        if (!exact_keys(request, request_keys)
            || !request.at("schema").is_string()
            || request.at("schema").get<std::string>() != resume_context_snapshot_schema
            || !request.at("content_type").is_string()
            || !request.at("content").is_string()
            || !request.at("provenance").is_array()) {
            return {false, {}, "context request does not match snapshot schema"};
        }

        const auto content_type = request.at("content_type").get<std::string>();
        const auto content = request.at("content").get<std::string>();
        const auto& provenance = request.at("provenance");
        if (!allowed_content_type(content_type)) {
            return {false, {}, "context content type is not allowed"};
        }
        if (content.empty() || content.size() > max_content_bytes
            || !valid_utf8(content)) {
            return {false, {}, "context content must be 1..16384 valid UTF-8 bytes"};
        }
        if (provenance.empty() || provenance.size() > max_provenance_entries) {
            return {false, {}, "context provenance must contain 1..8 entries"};
        }

        static const std::set<std::string> provenance_keys = {
            "kind", "ref", "sha256"
        };
        for (const auto& entry : provenance) {
            if (!exact_keys(entry, provenance_keys)
                || !entry.at("kind").is_string()
                || !entry.at("ref").is_string()
                || !entry.at("sha256").is_string()) {
                return {false, {}, "context provenance entry is invalid"};
            }
            if (!allowed_provenance_kind(entry.at("kind").get<std::string>())) {
                return {false, {}, "context provenance kind is not allowed"};
            }
            if (!safe_ref_text(entry.at("ref").get<std::string>())) {
                return {false, {}, "context provenance ref is invalid"};
            }
            if (!lowercase_sha256(entry.at("sha256").get<std::string>())) {
                return {false, {}, "context provenance sha256 is invalid"};
            }
        }

        const Json canonical = {
            {"schema", resume_context_snapshot_schema},
            {"content_type", content_type},
            {"content", content},
            {"provenance", provenance}
        };
        return {true, canonical.dump(), {}};
    } catch (...) {
        return {false, {}, "context request is not valid strict JSON"};
    }
}

} // namespace gaudere_agent
