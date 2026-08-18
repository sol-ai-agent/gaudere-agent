#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"
#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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
        const auto now = [] { return std::chrono::system_clock::now(); };

        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::scheduling::wake::Runtime action_runtime(action_store, now);
        gaudere::work::Runtime work_runtime(task_store, now);
        gaudere::scheduling::wake::Scheduler work_scheduler;
        gaudere_agent::TaskExecutor task_executor(work_runtime, task_store);
        gaudere_agent::TaskDispatcher task_dispatcher(task_store, task_executor);
        gaudere_agent::WorkController work_controller(
            work_scheduler, work_runtime, task_dispatcher, "main-worker");

        action_runtime.recover();
        work_runtime.recover();
        if (!work_controller.start()) {
            throw std::runtime_error("cannot start work controller");
        }
        std::cout << "gaudere-agent: running\n";

        int received = 0;
        std::atomic_bool signal_wait_failed{false};
        std::thread signal_waiter;
        if (!options.check_only) {
            signal_waiter = std::thread([&] {
                int signal = 0;
                if (sigwait(&signals, &signal) != 0) {
                    signal_wait_failed.store(true);
                } else {
                    received = signal;
                }
                work_controller.stop();
            });
        }

        bool work_conflict = false;
        if (options.check_only) {
            work_controller.stop();
        }
        for (;;) {
            const auto result = work_controller.wait_and_run();
            if (result == gaudere_agent::WorkCycleResult::stopped) {
                break;
            }
            if (result == gaudere_agent::WorkCycleResult::state_conflict) {
                work_conflict = true;
                work_controller.stop();
            }
        }

        if (signal_waiter.joinable()) {
            signal_waiter.join();
        }
        if (received != 0) {
            std::cout << "gaudere-agent: shutdown requested by signal "
                      << received << '\n';
        }

        action_runtime.request_shutdown();
        const bool actions_safe = action_runtime.try_mark_safe();
        const bool work_safe = work_runtime.try_mark_safe();
        if (signal_wait_failed.load()) {
            std::cerr << "gaudere-agent: cannot wait for shutdown signal\n";
            return 1;
        }
        if (work_conflict) {
            std::cerr << "gaudere-agent: work controller state conflict\n";
            return 2;
        }
        if (!actions_safe || !work_safe) {
            std::cerr << "gaudere-agent: unsafe to stop; running work remains\n";
            return 2;
        }

        std::cout << "gaudere-agent: safe\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-agent: " << error.what() << '\n';
        return 1;
    }
}
