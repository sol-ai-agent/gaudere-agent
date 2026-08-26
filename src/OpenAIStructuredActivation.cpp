#include "OpenAIStructuredActivation.hpp"

#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace gaudere_agent {
namespace {

bool valid_model_name(const std::string_view value) noexcept
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char character : value) {
        const bool alpha_numeric =
            (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
        if (!alpha_numeric && character != '.' && character != '-'
            && character != '_' && character != ':') {
            return false;
        }
    }
    return true;
}

} // namespace

OpenAIStructuredActivation::OpenAIStructuredActivation(
    gaudere::scheduling::wake::Runtime& action_runtime,
    gaudere::scheduling::wake::ActionStore& action_store,
    gaudere::budget::Store& budget_store,
    OpenAIJsonSchemaContract contract,
    std::string model,
    std::string secret_name,
    std::string secret_directory)
    : model_(std::move(model)),
      secret_name_(std::move(secret_name)),
      budget_policy_(openai_bootstrap_budget_policy()),
      secrets_(std::move(secret_directory)),
      curl_global_(),
      transport_(curl_global_),
      structured_transport_(transport_, std::move(contract)),
      provider_(structured_transport_, secrets_, model_, secret_name_),
      handler_(action_runtime, action_store, provider_, budget_store,
               budget_policy_, [] { return std::chrono::system_clock::now(); })
{
    if (!valid_model_name(model_)) {
        throw std::runtime_error("configured OpenAI model name is invalid");
    }
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
