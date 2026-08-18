#include "LocalEchoHandler.hpp"
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
    bool echo = false;
    std::string echo_id;
    std::string echo_text;
};

void usage(const char* program)
{
    std::cout << "Usage: " << program
              << " --state PATH [--check | --echo ID TEXT]\n";
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
        } else if (argument == "--echo" && index + 2 < argc) {
            options.echo = true;
            options.echo_id = argv[++index];
            options.echo_text = argv[++index];
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
    if (options.check_only && options.echo) {
        throw std::invalid_argument("--check and --echo are mutually exclusive");
    }
    if (options.echo && options.echo_id.empty()) {
        throw std::invalid_argument("--echo ID must not be empty");
    }
    return options;
}

sigset_t block_control_signals()
{
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) {
        throw std::runtime_error("cannot block control signals");
    }
    return signals;
}

gaudere::work::Task make_echo_task(const Options& options)
{
    gaudere::work::Task task;
    task.id = options.echo_id;
    task.idempotency_key = "local.echo:" + options.echo_id;
    task.kind = "local.echo";
    task.input_content_type = "text/plain";
    task.input = options.echo_text;
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = std::chrono::seconds{1};
    task.limits.max_attempts = 1;
    return task;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        const auto signals = block_control_signals();
        const auto now = [] { return std::chrono::system_clock::now(); };

        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::scheduling::wake::Runtime action_runtime(action_store, now);
        gaudere::work::Runtime work_runtime(task_store, now);
        gaudere::scheduling::wake::Scheduler work_scheduler;
        gaudere_agent::TaskExecutor task_executor(work_runtime, task_store);
        gaudere_agent::TaskDispatcher task_dispatcher(task_store, task_executor);
        gaudere_agent::LocalEchoHandler echo_handler;
        gaudere_agent::WorkController work_controller(
            work_scheduler, work_runtime, task_dispatcher, "main-worker");

        if (!task_dispatcher.register_handler("local.echo", echo_handler)) {
            throw std::runtime_error("cannot register local.echo handler");
        }

        action_runtime.recover();
        work_runtime.recover();
        if (!work_controller.start()) {
            throw std::runtime_error("cannot start work controller");
        }
        std::cout << "gaudere-agent: running\n";

        if (options.echo) {
            const auto submit = work_runtime.submit(make_echo_task(options));
            if (submit != gaudere::work::SubmitResult::accepted
                && submit != gaudere::work::SubmitResult::duplicate) {
                throw std::runtime_error("local.echo submission rejected");
            }
            work_controller.notify_work();
            const auto cycle = work_controller.wait_and_run();
            if (cycle == gaudere_agent::WorkCycleResult::state_conflict
                || cycle == gaudere_agent::WorkCycleResult::stopped) {
                throw std::runtime_error("local.echo dispatch failed");
            }
            const auto stored = task_store.find(options.echo_id);
            if (!stored || stored->status != gaudere::work::TaskStatus::succeeded
                || !stored->result) {
                throw std::runtime_error("local.echo did not complete successfully");
            }
            std::cout << "gaudere-agent: echo result: " << stored->result->output << '\n';
            work_controller.stop();
            static_cast<void>(work_controller.wait_and_run());
        } else {
            int received = 0;
            std::atomic_bool signal_wait_failed{false};
            std::atomic_bool internal_wake{false};
            std::thread signal_waiter;
            if (!options.check_only) {
                signal_waiter = std::thread([&] {
                    int signal = 0;
                    if (sigwait(&signals, &signal) != 0) {
                        signal_wait_failed.store(true);
                    } else if (signal != SIGUSR1) {
                        received = signal;
                    } else if (!internal_wake.load()) {
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
                    if (signal_waiter.joinable()) {
                        internal_wake.store(true);
                        if (pthread_kill(signal_waiter.native_handle(), SIGUSR1) != 0) {
                            signal_wait_failed.store(true);
                        }
                    }
                }
            }

            if (signal_waiter.joinable()) {
                signal_waiter.join();
            }
            if (received == SIGINT || received == SIGTERM) {
                std::cout << "gaudere-agent: shutdown requested by signal "
                          << received << '\n';
            } else if (received == SIGUSR1) {
                std::cout << "gaudere-agent: shutdown requested by control signal "
                          << received << '\n';
            }

            if (signal_wait_failed.load()) {
                std::cerr << "gaudere-agent: cannot coordinate shutdown signal\n";
                return 1;
            }
            if (work_conflict) {
                std::cerr << "gaudere-agent: work controller state conflict\n";
                return 2;
            }
        }

        action_runtime.request_shutdown();
        const bool actions_safe = action_runtime.try_mark_safe();
        const bool work_safe = work_runtime.try_mark_safe();
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
