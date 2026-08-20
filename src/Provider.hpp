#ifndef GAUDERE_AGENT_PROVIDER_HPP
#define GAUDERE_AGENT_PROVIDER_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace gaudere_agent {

enum class ProviderOutcome {
    succeeded,
    rejected,
    effect_unknown
};

struct ProviderRequest {
    std::string idempotency_key;
    std::string content_type;
    std::string input;
    std::uint64_t max_output_bytes = 0;
    std::chrono::milliseconds max_runtime{0};
};

struct ProviderResult {
    ProviderResult() = default;

    ProviderResult(ProviderOutcome outcome_value,
                   std::string content_type_value,
                   std::string output_value,
                   std::string failure_code_value,
                   std::string failure_message_value,
                   std::string metadata_content_type_value = {},
                   std::string metadata_value = {})
        : outcome(outcome_value),
          content_type(std::move(content_type_value)),
          output(std::move(output_value)),
          failure_code(std::move(failure_code_value)),
          failure_message(std::move(failure_message_value)),
          metadata_content_type(std::move(metadata_content_type_value)),
          metadata(std::move(metadata_value))
    {
    }

    ProviderOutcome outcome = ProviderOutcome::effect_unknown;
    std::string content_type;
    std::string output;
    std::string failure_code;
    std::string failure_message;
    // Provider implementations may expose a small normalized machine-readable
    // record separately from user-visible output. Raw provider envelopes must not
    // be copied here.
    std::string metadata_content_type;
    std::string metadata;
};

/** Provider-neutral synchronous outbound call boundary.
 *
 * ProviderTaskHandler checks cancellation before invoking this interface. Once
 * invoke() begins, the corresponding recoverable Action has already been marked
 * as an unknown external effect. Implementations must therefore report any
 * ambiguous interruption or transport result as effect_unknown rather than
 * claiming normal cancellation.
 */
class Provider {
public:
    virtual ~Provider() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ProviderResult invoke(const ProviderRequest& request) = 0;
};

} // namespace gaudere_agent

#endif
