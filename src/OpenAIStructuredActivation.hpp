#ifndef GAUDERE_AGENT_OPENAI_STRUCTURED_ACTIVATION_HPP
#define GAUDERE_AGENT_OPENAI_STRUCTURED_ACTIVATION_HPP

#include "CurlHttpTransport.hpp"
#include "FileSecretSource.hpp"
#include "OpenAIBudget.hpp"
#include "OpenAIJsonSchemaTransport.hpp"
#include "OpenAIResponsesProvider.hpp"
#include "ProviderTaskHandler.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <string>

namespace gaudere_agent {

/** Explicit OpenAI lifetime boundary with one immutable Structured Output schema.
 *
 * This is deliberately separate from OpenAIActivation so generic/production text
 * paths cannot acquire Structured Output behavior implicitly. Construction does
 * secret/schema preflight only; provider budget and Action authority remain in the
 * existing ProviderTaskHandler.
 */
class OpenAIStructuredActivation final {
public:
    OpenAIStructuredActivation(
        gaudere::scheduling::wake::Runtime& action_runtime,
        gaudere::scheduling::wake::ActionStore& action_store,
        gaudere::budget::Store& budget_store,
        OpenAIJsonSchemaContract contract,
        std::string model,
        std::string secret_name = "openai-api-key",
        std::string secret_directory = "/run/secrets");

    OpenAIStructuredActivation(const OpenAIStructuredActivation&) = delete;
    OpenAIStructuredActivation& operator=(const OpenAIStructuredActivation&) = delete;
    OpenAIStructuredActivation(OpenAIStructuredActivation&&) = delete;
    OpenAIStructuredActivation& operator=(OpenAIStructuredActivation&&) = delete;

    [[nodiscard]] TaskHandler& handler() noexcept { return handler_; }

private:
    std::string model_;
    std::string secret_name_;
    gaudere::budget::Policy budget_policy_;
    FileSecretSource secrets_;
    CurlGlobal curl_global_;
    CurlHttpTransport transport_;
    OpenAIJsonSchemaTransport structured_transport_;
    OpenAIResponsesProvider provider_;
    ProviderTaskHandler handler_;
};

} // namespace gaudere_agent

#endif
