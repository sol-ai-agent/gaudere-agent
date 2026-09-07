#include "GooseGovernanceStore.hpp"
#include "GooseToolsMcp.hpp"
#include "LiveControl.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string socket_path;
    std::string governance_path;
};

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--socket" && i + 1 < argc) {
            options.socket_path = argv[++i];
        } else if (argument == "--governance" && i + 1 < argc) {
            options.governance_path = argv[++i];
        } else {
            throw std::invalid_argument("unknown or incomplete Goose MCP argument");
        }
    }
    if (options.socket_path.empty() || options.socket_path.front() != '/') {
        throw std::invalid_argument("--socket requires an absolute path");
    }
    if (options.governance_path.empty() || options.governance_path.front() != '/') {
        throw std::invalid_argument("--governance requires an absolute path");
    }
    return options;
}

bool bounded_line(std::string& line)
{
    constexpr std::size_t max_bytes = 128 * 1024;
    if (!std::getline(std::cin, line)) return false;
    if (line.size() > max_bytes) {
        throw std::runtime_error("MCP request exceeds byte limit");
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        gaudere_agent::GooseGovernanceStore governance(options.governance_path);
        gaudere_agent::GooseToolsMcp server(
            governance,
            [&](const gaudere_agent::LiveControlCommand& command) {
                std::ostringstream output;
                std::ostringstream error;
                const int code = gaudere_agent::run_live_control_client(
                    options.socket_path, command, output, error);
                return gaudere_agent::LiveControlReply{
                    code == 0,
                    code,
                    code == 0 ? output.str() : error.str()};
            });

        std::string line;
        while (bounded_line(line)) {
            if (line.empty()) continue;
            nlohmann::json request;
            try {
                request = nlohmann::json::parse(line);
            } catch (const std::exception&) {
                const nlohmann::json response = {
                    {"jsonrpc", "2.0"},
                    {"id", nullptr},
                    {"error", nlohmann::json{{"code", -32700},
                                             {"message", "Parse error"}}}
                };
                std::cout << response.dump() << '\n' << std::flush;
                continue;
            }
            const auto response = server.process(request);
            if (response) {
                std::cout << response->dump() << '\n' << std::flush;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-goose-tools-mcp: " << error.what() << '\n';
        return 1;
    }
}
