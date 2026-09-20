#include "LocalGooseCycleStimulusStore.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* program)
{
    std::cerr << "Usage: " << program
              << " --stimulus-sidecar PATH\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--stimulus-sidecar") {
            usage(argv[0]);
            return 2;
        }

        const std::string path = argv[2];
        if (path.empty() || path.front() != '/') {
            throw std::invalid_argument(
                "stimulus sidecar path must be absolute");
        }
        if (std::filesystem::exists(path)) {
            throw std::invalid_argument(
                "stimulus sidecar already exists; refusing initializer replay");
        }

        {
            gaudere_agent::LocalGooseCycleStimulusStore store(path);
        }
        const auto inspection =
            gaudere_agent::inspect_local_goose_cycle_stimulus_sidecar(path);
        if (!inspection.eligible || !inspection.stimuli.empty()) {
            throw std::runtime_error(
                "initialized stimulus sidecar is not canonical and empty");
        }

        std::cout
            << "schema=gaudere.local-goose-cycle-stimulus-init.v1\n"
            << "stimuli=0\n"
            << "LOCAL_GOOSE_STIMULUS_INIT=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-cycle-stimulus-init: "
                  << error.what() << '\n';
        return 1;
    }
}
