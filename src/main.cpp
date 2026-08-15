#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string state_path;
    bool check_only = false;
};

void usage(const char* program)
{
    std::cout << "Usage: " << program << " --state PATH [--check]\n";
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--check") {
            options.check_only = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }
    if (options.state_path.empty()) {
        throw std::invalid_argument("--state PATH is required");
    }
    return options;
}

sigset_t block_shutdown_signals()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("cannot block shutdown signals");
    }
    return signals;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        const auto signals = block_shutdown_signals();

        gaudere::persistence::sqlite::ActionStore store(options.state_path);
        gaudere::scheduling::wake::Runtime runtime(
            store, [] { return std::chrono::system_clock::now(); });

        runtime.recover();
        std::cout << "gaudere-agent: running\n";

        if (!options.check_only) {
            int received = 0;
            if (sigwait(&signals, &received) != 0) {
                throw std::runtime_error("cannot wait for shutdown signal");
            }
            std::cout << "gaudere-agent: shutdown requested by signal "
                      << received << '\n';
        }

        runtime.request_shutdown();
        if (!runtime.try_mark_safe()) {
            std::cerr << "gaudere-agent: unsafe to stop; running actions remain\n";
            return 2;
        }

        std::cout << "gaudere-agent: safe\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-agent: " << error.what() << '\n';
        return 1;
    }
}
