#ifndef GAUDERE_AGENT_OPENAI_ACTIVATION_HPP
#define GAUDERE_AGENT_OPENAI_ACTIVATION_HPP

#include "CurlHttpTransport.hpp"
#include "FileSecretSource.hpp"
#include "OpenAIResponsesProvider.hpp"
#include "ProviderTaskHandler.hpp"

#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <string>

namespace gaudere_agent {

/** Explicit lifetime boundary for the inactive-by-default OpenAI provider stack.
 *
 * Construction opens the configured secret directory and performs a credential
 * preflight. It creates no Task and performs no network request. The returned
 * handler becomes externally capable only if the caller explicitly registers it
 * and the surrounding container also has outbound networking.
 */
class OpenAIActivation final {
public:
    OpenAIActivation(gaudere::scheduling::wake::Runtime& action_runtime,
                     gaudere::scheduling::wake::ActionStore& action_store,
                     std::string model,
                     std::string secret_name = "openai-api-key",
                     std::string secret_directory = "/run/secrets");

    OpenAIActivation(const OpenAIActivation&) = delete;
    OpenAIActivation& operator=(const OpenAIActivation&) = delete;
    OpenAIActivation(OpenAIActivation&&) = delete;
    OpenAIActivation& operator=(OpenAIActivation&&) = delete;

    [[nodiscard]] TaskHandler& handler() noexcept { return handler_; }
    [[nodiscard]] const std::string& model() const noexcept { return model_; }
    [[nodiscard]] const std::string& secret_name() const noexcept
    {
        return secret_name_;
    }

private:
    std::string model_;
    std::string secret_name_;
    FileSecretSource secrets_;
    CurlGlobal curl_global_;
    CurlHttpTransport transport_;
    OpenAIResponsesProvider provider_;
    ProviderTaskHandler handler_;
};

} // namespace gaudere_agent

#endif
