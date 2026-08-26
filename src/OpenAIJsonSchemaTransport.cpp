#include "OpenAIJsonSchemaTransport.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;
constexpr std::size_t max_schema_bytes = 64 * 1024;

bool valid_name(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 64) return false;
    for (const unsigned char character : value) {
        if (!(std::isalnum(character) || character == '_' || character == '-')) {
            return false;
        }
    }
    return true;
}

std::string canonical_schema(const std::string& schema_json)
{
    if (schema_json.empty() || schema_json.size() > max_schema_bytes) {
        throw std::invalid_argument(
            "OpenAI JSON schema must contain 1..65536 bytes");
    }
    Json schema;
    try {
        schema = Json::parse(schema_json);
    } catch (...) {
        throw std::invalid_argument("OpenAI JSON schema must be valid JSON");
    }
    if (!schema.is_object()) {
        throw std::invalid_argument("OpenAI JSON schema root must be an object");
    }
    return schema.dump();
}

} // namespace

OpenAIJsonSchemaTransport::OpenAIJsonSchemaTransport(
    HttpTransport& downstream,
    OpenAIJsonSchemaContract contract)
    : downstream_(downstream),
      name_(std::move(contract.name)),
      canonical_schema_(canonical_schema(contract.schema_json))
{
    if (!valid_name(name_)) {
        throw std::invalid_argument(
            "OpenAI Structured Output name must be 1..64 alphanumeric, underscore or hyphen characters");
    }
}

HttpTransportResult OpenAIJsonSchemaTransport::perform(
    const HttpRequest& request,
    std::optional<HttpSensitiveHeader> sensitive_header)
{
    Json body;
    try {
        body = Json::parse(request.body);
    } catch (...) {
        throw std::invalid_argument(
            "OpenAI Structured Output transport requires a JSON request body");
    }
    if (!body.is_object()) {
        throw std::invalid_argument(
            "OpenAI Structured Output transport requires an object request body");
    }
    if (body.contains("text")) {
        throw std::invalid_argument(
            "OpenAI request already contains text configuration");
    }

    body["text"] = Json{
        {"format", Json{
            {"type", "json_schema"},
            {"name", name_},
            {"strict", true},
            {"schema", Json::parse(canonical_schema_)}
        }}
    };

    HttpRequest structured = request;
    structured.body = body.dump();
    return downstream_.perform(structured, std::move(sensitive_header));
}

} // namespace gaudere_agent
