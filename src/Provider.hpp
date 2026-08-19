#ifndef GAUDERE_AGENT_PROVIDER_HPP
#define GAUDERE_AGENT_PROVIDER_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

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
    ProviderOutcome outcome = ProviderOutcome::effect_unknown;
    std::string content_type;
    std::string output;
    std::string failure_code;
    std::string failure_message;
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
