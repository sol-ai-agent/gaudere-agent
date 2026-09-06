#include "LocalActivityPulseStatus.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Arguments {
    std::string state_path;
    std::string sidecar_path;
    bool enabled = false;
};

Arguments parse_arguments(const int argc, char** argv)
{
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string item = argv[index];
        if (item == "--state") {
            if (++index >= argc) throw std::invalid_argument("--state requires a path");
            arguments.state_path = argv[index];
        } else if (item == "--sidecar") {
            if (++index >= argc) throw std::invalid_argument("--sidecar requires a path");
            arguments.sidecar_path = argv[index];
        } else if (item == "--enabled") {
            arguments.enabled = true;
        } else if (item == "--disabled") {
            arguments.enabled = false;
        } else {
            throw std::invalid_argument("unknown argument: " + item);
        }
    }
    if (arguments.state_path.empty()) throw std::invalid_argument("--state is required");
    if (arguments.sidecar_path.empty()) throw std::invalid_argument("--sidecar is required");
    return arguments;
}

} // namespace

int main(const int argc, char** argv)
{
    try {
        const auto arguments = parse_arguments(argc, argv);
        const auto status = gaudere_agent::inspect_local_activity_pulse_status(
            arguments.state_path, arguments.sidecar_path, arguments.enabled);
        if (!status.eligible) {
            std::cerr << "gaudere-local-activity-status: " << status.detail << '\n';
            return 2;
        }
        std::cout << status.canonical_json << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-activity-status: " << error.what() << '\n';
        return 64;
    }
}
