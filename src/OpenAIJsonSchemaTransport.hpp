#ifndef GAUDERE_AGENT_OPENAI_JSON_SCHEMA_TRANSPORT_HPP
#define GAUDERE_AGENT_OPENAI_JSON_SCHEMA_TRANSPORT_HPP

#include "HttpTransport.hpp"

#include <string>

namespace gaudere_agent {

struct OpenAIJsonSchemaContract {
    std::string name;
    std::string schema_json;
};

/** OpenAI-specific HTTP decorator for one immutable Structured Output contract.
 *
 * The wrapped OpenAIResponsesProvider remains unchanged. This decorator rewrites
 * only its already-bounded JSON request body by adding Responses API `text.format`
 * with `type=json_schema` and `strict=true`, then delegates exactly once to the
 * borrowed transport. It owns no secret, provider budget, Action, Task or network
 * implementation.
 */
class OpenAIJsonSchemaTransport final : public HttpTransport {
public:
    OpenAIJsonSchemaTransport(HttpTransport& downstream,
                              OpenAIJsonSchemaContract contract);

    [[nodiscard]] HttpTransportResult perform(
        const HttpRequest& request,
        std::optional<HttpSensitiveHeader> sensitive_header) override;

private:
    HttpTransport& downstream_;
    std::string name_;
    std::string canonical_schema_;
};

} // namespace gaudere_agent

#endif
