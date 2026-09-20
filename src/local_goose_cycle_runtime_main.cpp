#include "LiveControl.hpp"
#include "LiveControlProcessor.hpp"
#include "LocalGooseCycleHandler.hpp"
#include "LocalGooseCycleSchedulerBridge.hpp"
#include "LocalGooseCycleService.hpp"
#include "LocalGooseCycleStore.hpp"
#include "LocalGooseCycleStimulusControl.hpp"
#include "LocalGooseCycleStimulusService.hpp"
#include "LocalGooseCycleStimulusStore.hpp"
#include "LocalGooseRunner.hpp"
#include "OpenAIBudget.hpp"
#include "StateLock.hpp"
#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"
#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string state_path;
    std::string cycle_sidecar;
    std::string stimulus_sidecar;
    std::string model;
    std::string model_sha256;
    std::string governance;
    std::string control_socket;
    bool check_only = false;
    bool once = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " --state PATH --cycle-sidecar PATH"
        << " [--stimulus-sidecar PATH]"
        << " --model MODEL --model-sha256 SHA256"
        << " --governance PATH --control-socket PATH [--check | --once]\n";
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isxdigit(c) || (c >= 'A' && c <= 'F')) return false;
    }
    return true;
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--state" && index + 1 < argc) {
            options.state_path = argv[++index];
        } else if (argument == "--cycle-sidecar" && index + 1 < argc) {
            options.cycle_sidecar = argv[++index];
        } else if (argument == "--stimulus-sidecar" && index + 1 < argc) {
            options.stimulus_sidecar = argv[++index];
        } else if (argument == "--model" && index + 1 < argc) {
            options.model = argv[++index];
        } else if (argument == "--model-sha256" && index + 1 < argc) {
            options.model_sha256 = argv[++index];
        } else if (argument == "--governance" && index + 1 < argc) {
            options.governance = argv[++index];
        } else if (argument == "--control-socket" && index + 1 < argc) {
            options.control_socket = argv[++index];
        } else if (argument == "--check") {
            options.check_only = true;
        } else if (argument == "--once") {
            options.once = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + argument);
        }
    }

    if (options.state_path.empty() || options.cycle_sidecar.empty()
        || options.model.empty() || options.model_sha256.empty()
        || options.governance.empty() || options.control_socket.empty()) {
        throw std::invalid_argument(
            "state, cycle sidecar, model, model SHA256, governance and control socket are required");
    }
    if (options.state_path.front() != '/' || options.cycle_sidecar.front() != '/'
        || (!options.stimulus_sidecar.empty()
            && options.stimulus_sidecar.front() != '/')
        || options.model.front() != '/' || options.governance.front() != '/'
        || options.control_socket.front() != '/') {
        throw std::invalid_argument(
            "runtime paths and Local Goose model selector must use absolute form");
    }
    if (!canonical_sha256(options.model_sha256)) {
        throw std::invalid_argument(
            "model SHA256 must be 64 lowercase hexadecimal characters");
    }
    if (options.check_only && options.once) {
        throw std::invalid_argument("--check and --once are mutually exclusive");
    }
    return options;
}

void require_regular_non_symlink(const std::string& path,
                                 const std::string& description)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            description + " must already exist as a regular non-symlink file");
    }
}

void require_distinct(const std::string& left,
                      const std::string& right,
                      const std::string& detail)
{
    const auto left_path = std::filesystem::weakly_canonical(left);
    const auto right_path = std::filesystem::weakly_canonical(right);
    if (left_path == right_path) throw std::invalid_argument(detail);

    std::error_code error;
    if (std::filesystem::equivalent(left, right, error)) {
        throw std::invalid_argument(detail);
    }
    if (error) {
        throw std::runtime_error("could not compare runtime path identity");
    }
}

const char* state_name(const gaudere_agent::LocalGooseCycleState state) noexcept
{
    using State = gaudere_agent::LocalGooseCycleState;
    switch (state) {
    case State::dormant:
        return "dormant";
    case State::scheduled:
        return "scheduled";
    case State::prepared:
        return "prepared";
    case State::blocked:
        return "blocked";
    }
    return "unknown";
}

void validate_files(const Options& options)
{
    // options.model is the absolute-form Goose registry selector already used
    // by the existing Local Goose runtime (for example /vendor/model:quant).
    // The model weights are resolved by Goose below goose_path_root; the
    // selector itself is deliberately not required to exist as a filesystem
    // object.
    require_regular_non_symlink(options.state_path, "state database");
    require_regular_non_symlink(options.cycle_sidecar, "Local Goose cycle sidecar");
    require_regular_non_symlink(options.governance, "Local Goose governance sidecar");
    if (!options.stimulus_sidecar.empty()) {
        require_regular_non_symlink(
            options.stimulus_sidecar,
            "Local Goose cycle stimulus sidecar");
    }

    require_distinct(options.state_path, options.cycle_sidecar,
                     "Local Goose cycle sidecar must be distinct from state database");
    require_distinct(options.state_path, options.governance,
                     "Local Goose governance sidecar must be distinct from state database");
    require_distinct(options.cycle_sidecar, options.governance,
                     "Local Goose cycle and governance sidecars must be distinct");
    if (!options.stimulus_sidecar.empty()) {
        require_distinct(
            options.state_path, options.stimulus_sidecar,
            "Local Goose stimulus sidecar must be distinct from state database");
        require_distinct(
            options.cycle_sidecar, options.stimulus_sidecar,
            "Local Goose stimulus and cycle sidecars must be distinct");
        require_distinct(
            options.governance, options.stimulus_sidecar,
            "Local Goose stimulus and governance sidecars must be distinct");
        const auto stimulus_inspection =
            gaudere_agent::inspect_local_goose_cycle_stimulus_sidecar(
                options.stimulus_sidecar);
        if (!stimulus_inspection.eligible) {
            throw std::invalid_argument(
                "Local Goose cycle stimulus sidecar is not eligible: "
                + stimulus_inspection.detail);
        }
    }

    const auto inspection = gaudere_agent::inspect_local_goose_cycle_sidecar(
        options.cycle_sidecar);
    if (!inspection.eligible || !inspection.cursor) {
        throw std::invalid_argument(
            "Local Goose cycle sidecar is not eligible: " + inspection.detail);
    }
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

bool requires_immediate_recovery(
    const gaudere_agent::LocalGooseCycleServiceResult result) noexcept
{
    using Result = gaudere_agent::LocalGooseCycleServiceResult;
    return result == Result::prepared
        || result == Result::submitted
        || result == Result::executed;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;
        validate_files(options);

        const auto initial = gaudere_agent::inspect_local_goose_cycle_sidecar(
            options.cycle_sidecar);
        if (!initial.cursor) {
            throw std::runtime_error("validated Local Goose cycle cursor disappeared");
        }
        if (options.check_only) {
            std::cout
                << "gaudere-local-goose-cycle-runtime: ready state="
                << state_name(initial.cursor->state)
                << " generation=" << initial.cursor->generation
                << " provider_execution=false automatic_seed=false\n";
            return 0;
        }

        const auto signals = block_control_signals();
        gaudere_agent::StateLock state_lock(options.state_path);
        const auto now = [] { return std::chrono::system_clock::now(); };
        const auto now_ms = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        };

        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);
        gaudere::work::Runtime work_runtime(task_store, now);
        gaudere::scheduling::wake::Scheduler scheduler;
        gaudere_agent::TaskExecutor task_executor(work_runtime, task_store);
        gaudere_agent::TaskDispatcher task_dispatcher(task_store, task_executor);
        gaudere_agent::WorkController work_controller(
            scheduler, work_runtime, task_dispatcher, "local-goose-cycle-runtime");

        gaudere_agent::LocalGooseCycleStore cycle_store(options.cycle_sidecar);
        std::unique_ptr<gaudere_agent::LocalGooseCycleStimulusStore>
            stimulus_store;
        std::unique_ptr<gaudere_agent::LocalGooseCycleStimulusService>
            stimulus_service;
        std::unique_ptr<gaudere_agent::LocalGooseCycleStimulusControl>
            stimulus_control;
        if (!options.stimulus_sidecar.empty()) {
            stimulus_store =
                std::make_unique<gaudere_agent::LocalGooseCycleStimulusStore>(
                    options.stimulus_sidecar);
            stimulus_service =
                std::make_unique<gaudere_agent::LocalGooseCycleStimulusService>(
                    *stimulus_store, cycle_store, task_store);
            stimulus_control =
                std::make_unique<gaudere_agent::LocalGooseCycleStimulusControl>(
                    *stimulus_service, now_ms);
        }

        gaudere_agent::LiveControlProcessor::LocalGooseStimulus
            stimulus_callback;
        if (stimulus_control) {
            stimulus_callback = [&stimulus_control](const std::string& request_id) {
                return stimulus_control->stimulate(request_id);
            };
        }

        gaudere_agent::LiveControlMailbox control_mailbox;
        gaudere_agent::LiveControlProcessor control_processor(
            work_runtime, task_store, budget_store,
            gaudere_agent::openai_bootstrap_budget_policy(), false, nullptr,
            [&scheduler] { return scheduler.next(); },
            std::move(stimulus_callback));
        gaudere_agent::LiveControlServer control_server(
            options.control_socket, control_mailbox,
            [&work_controller] { work_controller.interrupt(); });

        std::atomic_bool stop_requested{false};
        const auto pump_local_goose_tools = [&]() {
            if (stop_requested.load()) {
                throw std::runtime_error(
                    "shutdown requested during Local Goose cycle cognition");
            }
            const auto control = control_processor.process(control_mailbox);
            if (control.work_may_be_pending) work_controller.notify_work();
            if (control.wake_deadline_may_have_changed)
                work_controller.refresh_deadlines();
            if (control.local_goose_cycle_may_have_changed)
                work_controller.interrupt();
        };

        gaudere_agent::PosixLocalGooseRunner runner(pump_local_goose_tools);
        gaudere_agent::LocalGooseCycleHandler handler(
            runner, options.model, options.model_sha256, true,
            options.control_socket, options.governance);
        gaudere_agent::LocalGooseCycleService cycle_service(
            cycle_store, task_store, work_runtime, handler,
            options.model_sha256, now_ms);
        gaudere_agent::LocalGooseCycleSchedulerBridge cycle_scheduler(scheduler);

        work_runtime.recover();
        if (!work_controller.start()) {
            throw std::runtime_error("cannot start Local Goose cycle work controller");
        }
        if (!control_server.start()) {
            throw std::runtime_error("cannot start Local Goose cycle control server");
        }

        bool cycle_monitoring = true;
        bool once_terminal = false;
        bool once_succeeded = false;
        const auto step_cycle = [&]() {
            const auto step = cycle_service.step();
            auto cursor = step.cursor;
            if (!cursor) cursor = cycle_store.find(gaudere_agent::local_goose_cycle_scope);

            const auto arm = cycle_scheduler.arm(cursor);
            if (arm == gaudere_agent::LocalGooseCycleSchedulerArmResult::invalid) {
                std::cerr
                    << "gaudere-local-goose-cycle-runtime: invalid durable scheduler projection\n";
                cycle_monitoring = false;
                return false;
            }

            if (step.task) {
                std::cout << "gaudere-local-goose-cycle-runtime: task="
                          << step.task->id << '\n';
            }
            if (step.decision) {
                std::cout << "gaudere-local-goose-cycle-runtime: decision="
                          << step.decision->decision << '\n';
                if (options.once) {
                    once_terminal = true;
                    once_succeeded = true;
                }
            }
            if (options.once
                && step.result == gaudere_agent::LocalGooseCycleServiceResult::blocked) {
                once_terminal = true;
                once_succeeded = false;
            }
            if (!step.detail.empty()) {
                std::cout << "gaudere-local-goose-cycle-runtime: "
                          << step.detail << '\n';
            }
            if (!step.healthy) {
                std::cerr
                    << "gaudere-local-goose-cycle-runtime: monitoring disabled\n";
                cycle_monitoring = false;
                return false;
            }

            cycle_monitoring = step.active;
            if (cursor) {
                if (cursor->state == gaudere_agent::LocalGooseCycleState::scheduled
                    && arm == gaudere_agent::LocalGooseCycleSchedulerArmResult::inactive) {
                    std::cerr
                        << "gaudere-local-goose-cycle-runtime: scheduled cursor lost its deadline\n";
                    cycle_monitoring = false;
                    return false;
                }
                if (cursor->state != gaudere_agent::LocalGooseCycleState::scheduled
                    && arm != gaudere_agent::LocalGooseCycleSchedulerArmResult::inactive) {
                    std::cerr
                        << "gaudere-local-goose-cycle-runtime: non-scheduled cursor retained a deadline\n";
                    cycle_monitoring = false;
                    return false;
                }
            }

            if (cycle_monitoring && requires_immediate_recovery(step.result)) {
                work_controller.interrupt();
            }
            return true;
        };

        int received = 0;
        std::atomic_bool signal_wait_failed{false};
        std::atomic_bool internal_wake{false};
        std::thread signal_waiter([&] {
            int signal = 0;
            if (sigwait(&signals, &signal) != 0) {
                signal_wait_failed.store(true);
            } else if (signal != SIGUSR1 || !internal_wake.load()) {
                received = signal;
            }
            stop_requested.store(true);
            work_controller.stop();
        });

        if (!step_cycle() || once_terminal) {
            stop_requested.store(true);
            work_controller.stop();
            internal_wake.store(true);
            if (pthread_kill(signal_waiter.native_handle(), SIGUSR1) != 0)
                signal_wait_failed.store(true);
        }

        std::cout
            << "gaudere-local-goose-cycle-runtime: running provider_execution=false automatic_seed=false\n";

        bool work_conflict = false;
        while (!stop_requested.load()) {
            const auto control = control_processor.process(control_mailbox);
            if (control.work_may_be_pending) work_controller.notify_work();
            if (control.wake_deadline_may_have_changed)
                work_controller.refresh_deadlines();
            if (control.local_goose_cycle_may_have_changed) {
                cycle_monitoring = true;
                if (!step_cycle() || once_terminal) {
                    stop_requested.store(true);
                    work_controller.stop();
                    internal_wake.store(true);
                    if (pthread_kill(
                            signal_waiter.native_handle(), SIGUSR1) != 0) {
                        signal_wait_failed.store(true);
                    }
                    continue;
                }
            }

            const auto result = work_controller.wait_and_run();
            if (result == gaudere_agent::WorkCycleResult::stopped) break;
            if (result == gaudere_agent::WorkCycleResult::state_conflict) {
                work_conflict = true;
                stop_requested.store(true);
                work_controller.stop();
                internal_wake.store(true);
                if (pthread_kill(signal_waiter.native_handle(), SIGUSR1) != 0)
                    signal_wait_failed.store(true);
                continue;
            }

            if (cycle_monitoring) {
                const bool cycle_ok = step_cycle();
                if (!cycle_ok || once_terminal) {
                    stop_requested.store(true);
                    work_controller.stop();
                    internal_wake.store(true);
                    if (pthread_kill(signal_waiter.native_handle(), SIGUSR1) != 0)
                        signal_wait_failed.store(true);
                }
            }
        }

        stop_requested.store(true);
        control_server.stop();
        control_server.join();
        if (signal_waiter.joinable()) signal_waiter.join();

        if (received == SIGINT || received == SIGTERM) {
            std::cout
                << "gaudere-local-goose-cycle-runtime: shutdown requested by signal "
                << received << '\n';
        }
        if (signal_wait_failed.load()) {
            std::cerr
                << "gaudere-local-goose-cycle-runtime: cannot coordinate shutdown signal\n";
            return 1;
        }
        if (work_conflict) {
            std::cerr
                << "gaudere-local-goose-cycle-runtime: work controller state conflict\n";
            return 2;
        }
        if (options.once && once_terminal && !once_succeeded) {
            std::cerr
                << "gaudere-local-goose-cycle-runtime: once cycle failed closed\n";
            return 3;
        }

        work_runtime.request_shutdown();
        if (!work_runtime.try_mark_safe()) {
            std::cerr
                << "gaudere-local-goose-cycle-runtime: unsafe to stop; running work remains\n";
            return 2;
        }

        if (options.once && once_succeeded) {
            std::cout << "gaudere-local-goose-cycle-runtime: once=complete\n";
        }
        std::cout << "gaudere-local-goose-cycle-runtime: safe\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-cycle-runtime: " << error.what() << '\n';
        return 1;
    }
}
