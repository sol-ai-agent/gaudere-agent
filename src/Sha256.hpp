#ifndef GAUDERE_AGENT_SHA256_HPP
#define GAUDERE_AGENT_SHA256_HPP

#include <string>
#include <string_view>

namespace gaudere_agent {

[[nodiscard]] std::string sha256_hex(std::string_view input);

} // namespace gaudere_agent

#endif
