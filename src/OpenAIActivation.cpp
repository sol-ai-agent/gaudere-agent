#include "OpenAIActivation.hpp"

#include <stdexcept>
#include <utility>

namespace gaudere_agent {

OpenAIActivation::OpenAIActivation(
    gaudere::scheduling::wake::Runtime& action_runtime,
    gaudere::scheduling::wake::ActionStore& action_store,
    std::string model,
    std::string secret_name,
    std::string secret_directory)
    : model_(std::move(model)),
      secret_name_(std::move(secret_name)),
      secrets_(std::move(secret_directory)),
      curl_global_(),
      transport_(curl_global_),
      provider_(transport_, secrets_, model_, secret_name_),
      handler_(action_runtime, action_store, provider_)
{
    const auto credential = secrets_.load(secret_name_);
    if (!credential) {
        throw std::runtime_error("configured OpenAI API secret is missing");
    }
    if (!OpenAIResponsesProvider::valid_api_key(credential->view())) {
        throw std::runtime_error(
            "configured OpenAI API secret is not a valid single-line Bearer value");
    }
}

} // namespace gaudere_agent
