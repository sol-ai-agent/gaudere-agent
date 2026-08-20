#ifndef GAUDERE_AGENT_OPENAI_BUDGET_HPP
#define GAUDERE_AGENT_OPENAI_BUDGET_HPP

#include <gaudere/budget/Store.hpp>

#include <chrono>
#include <string_view>

namespace gaudere_agent {

[[nodiscard]] inline std::string_view openai_budget_scope() noexcept
{
    return "provider.call:openai.responses";
}

[[nodiscard]] inline gaudere::budget::Policy openai_bootstrap_budget_policy() noexcept
{
    gaudere::budget::Policy policy;
    policy.max_total = 12;
    policy.max_in_window = 4;
    policy.window = std::chrono::hours{24};
    policy.min_interval = std::chrono::minutes{15};
    return policy;
}

} // namespace gaudere_agent

#endif
