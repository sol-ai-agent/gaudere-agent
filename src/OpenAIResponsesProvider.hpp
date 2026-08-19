#ifndef GAUDERE_AGENT_OPENAI_RESPONSES_PROVIDER_HPP
#define GAUDERE_AGENT_OPENAI_RESPONSES_PROVIDER_HPP

#include "HttpTransport.hpp"
#include "Provider.hpp"
#include "SecretSource.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace gaudere_agent {

/** Non-streaming OpenAI Responses adapter.
 *
 * The adapter itself owns no network implementation. It builds one bounded HTTP
 * request and delegates it to HttpTransport.
 */
class OpenAIResponsesProvider final : public Provider {
public:
    OpenAIResponsesProvider(
        HttpTransport& transport,
        SecretSource& secrets,
        std::string model,
        std::string secret_name = "openai-api-key",
        std::string endpoint = "https://api.openai.com/v1/responses");

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "openai.responses";
    }

    [[nodiscard]] ProviderResult invoke(const ProviderRequest& request) override;

    /** Validate the exact bytes that may be used as a Bearer API key.
     *
     * This is exposed so activation can reject a missing/newline-containing secret
     * before any provider Task or external-effect Action starts.
     */
    [[nodiscard]] static bool valid_api_key(std::string_view value) noexcept;
    [[nodiscard]] static std::string client_request_id(std::string_view key);

private:
    [[nodiscard]] static ProviderResult rejected(std::string code,
                                                 std::string message);
    [[nodiscard]] static ProviderResult unknown(std::string code,
                                                std::string message);
    [[nodiscard]] static std::uint64_t response_body_limit(
        std::uint64_t max_output_bytes) noexcept;

    HttpTransport& transport_;
    SecretSource& secrets_;
    std::string model_;
    std::string secret_name_;
    std::string endpoint_;
};

} // namespace gaudere_agent

#endif
