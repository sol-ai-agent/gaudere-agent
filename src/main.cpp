#include "AutonomousCognitionProviderGate.hpp"
#include "AutonomousCognitionProviderService.hpp"
#include "AutonomousCognitionPulse.hpp"
#include "AutonomousCognitionPulseService.hpp"
#include "AutonomousCognitionPulseStore.hpp"
#include "AutonomousCognitionStaleRefresh.hpp"
#include "BoundedReflection.hpp"
#include "CurrentCognitionHandler.hpp"
#include "ExplicitWake.hpp"
#include "LiveControl.hpp"
#include "LiveControlProcessor.hpp"
#include "LocalActivityPulse.hpp"
#include "LocalActivityPulseService.hpp"
#include "LocalActivityPulseStore.hpp"
#include "LocalEchoHandler.hpp"
#include "LocalWaitHandler.hpp"
#include "OpenAIActivation.hpp"
#include "OpenAIOneShot.hpp"
#include "OpenAIStructuredActivation.hpp"
#include "ResumeDecisionStructuredOutput.hpp"
#include "StateLock.hpp"
#include "TaskDispatcher.hpp"
#include "TaskExecutor.hpp"
#include "TaskReport.hpp"
#include "WorkController.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>
#include <gaudere/scheduling/wake/Scheduler.hpp>
#include <gaudere/scheduling/wake/WakeIntentRuntime.hpp>
#include <gaudere/work/Runtime.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string state_path;
    bool check_only = false;
    bool echo = false;
    bool enqueue_wait = false;
    bool enqueue_openai = false;
    bool openai_once = false;
    bool inspect_task = false;
    bool cancel_task = false;
    bool openai_enabled = false;
    bool wake_intents_enabled = false;
    bool autonomous_pulse_provider = false;
    bool openai_secret_explicit = false;
    bool secret_directory_explicit = false;
    std::string task_id;
    std::string text;
    std::string openai_model;
    std::string openai_secret = "openai-api-key";
    std::string secret_directory = "/run/secrets";
    std::string control_socket;
    std::string autonomous_pulse_sidecar;
    std::string local_activity_sidecar;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program << " --state PATH "
        << "[--check | --echo ID TEXT | --enqueue-wait ID MS | "
        << "--enqueue-openai ID TEXT | --openai-once ID TEXT | --task ID | "
        << "--cancel ID REASON] "
        << "[--control-socket PATH] "
        << "[--wake-intents] "
        << "[--autonomous-pulse-sidecar PATH] "
        << "[--autonomous-pulse-provider] "
        << "[--local-activity-sidecar PATH] "
        << "[--openai-model MODEL [--openai-secret NAME] [--secret-dir PATH]]\n";
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
            options.task_id = argv[++index];
            options.text = argv[++index];
        } else if (argument == "--enqueue-wait" && index + 2 < argc) {
            options.enqueue_wait = true;
            options.task_id = argv[++index];
            options.text = argv[++index];
        } else if (argument == "--enqueue-openai" && index + 2 < argc) {
            options.enqueue_openai = true;
            options.task_id = argv[++index];
            options.text = argv[++index];
        } else if (argument == "--openai-once" && index + 2 < argc) {
            options.openai_once = true;
            options.task_id = argv[++index];
            options.text = argv[++index];
        } else if (argument == "--task" && index + 1 < argc) {
            options.inspect_task = true;
            options.task_id = argv[++index];
        } else if (argument == "--cancel" && index + 2 < argc) {
            options.cancel_task = true;
            options.task_id = argv[++index];
            options.text = argv[++index];
        } else if (argument == "--control-socket" && index + 1 < argc) {
            options.control_socket = argv[++index];
        } else if (argument == "--autonomous-pulse-sidecar" && index + 1 < argc) {
            options.autonomous_pulse_sidecar = argv[++index];
        } else if (argument == "--autonomous-pulse-provider") {
            options.autonomous_pulse_provider = true;
        } else if (argument == "--local-activity-sidecar" && index + 1 < argc) {
            options.local_activity_sidecar = argv[++index];
        } else if (argument == "--wake-intents") {
            options.wake_intents_enabled = true;
        } else if (argument == "--openai-model" && index + 1 < argc) {
            options.openai_enabled = true;
            options.openai_model = argv[++index];
        } else if (argument == "--openai-secret" && index + 1 < argc) {
            options.openai_secret_explicit = true;
            options.openai_secret = argv[++index];
        } else if (argument == "--secret-dir" && index + 1 < argc) {
            options.secret_directory_explicit = true;
            options.secret_directory = argv[++index];
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

    const int modes = static_cast<int>(options.check_only)
        + static_cast<int>(options.echo)
        + static_cast<int>(options.enqueue_wait)
        + static_cast<int>(options.enqueue_openai)
        + static_cast<int>(options.openai_once)
        + static_cast<int>(options.inspect_task)
        + static_cast<int>(options.cancel_task);
    if (modes > 1) {
        throw std::invalid_argument(
            "--check, --echo, --enqueue-wait, --enqueue-openai, --openai-once, --task, and --cancel are mutually exclusive");
    }
    if (!options.control_socket.empty() && modes != 0) {
        throw std::invalid_argument("--control-socket is only valid in service mode");
    }
    if (!options.autonomous_pulse_sidecar.empty() && modes != 0) {
        throw std::invalid_argument(
            "--autonomous-pulse-sidecar is only valid in service mode");
    }
    if (!options.local_activity_sidecar.empty() && modes != 0) {
        throw std::invalid_argument(
            "--local-activity-sidecar is only valid in service mode");
    }
    if (options.autonomous_pulse_provider && modes != 0) {
        throw std::invalid_argument(
            "--autonomous-pulse-provider is only valid in service mode");
    }
    if (options.wake_intents_enabled && modes != 0 && !options.check_only) {
        throw std::invalid_argument(
            "--wake-intents is only valid in service mode or with --check");
    }

    if ((options.echo || options.enqueue_wait || options.enqueue_openai
         || options.openai_once || options.inspect_task || options.cancel_task)
        && options.task_id.empty()) {
        throw std::invalid_argument("task ID must not be empty");
    }
    if (options.cancel_task && options.text.empty()) {
        throw std::invalid_argument("cancellation reason must not be empty");
    }
    if ((options.enqueue_openai || options.openai_once) && options.text.empty()) {
        throw std::invalid_argument("OpenAI task input must not be empty");
    }
    if (options.enqueue_wait
        && !gaudere_agent::parse_local_wait_duration(options.text)) {
        throw std::invalid_argument(
            "--enqueue-wait MS must be an integer from 1 to 5000");
    }

    if (!options.openai_enabled
        && (options.openai_secret_explicit || options.secret_directory_explicit)) {
        throw std::invalid_argument(
            "--openai-secret and --secret-dir require --openai-model");
    }
    if (options.openai_once && !options.openai_enabled) {
        throw std::invalid_argument("--openai-once requires --openai-model");
    }
    if (options.autonomous_pulse_provider
        && options.autonomous_pulse_sidecar.empty()) {
        throw std::invalid_argument(
            "--autonomous-pulse-provider requires --autonomous-pulse-sidecar PATH");
    }
    if (options.autonomous_pulse_provider && !options.openai_enabled) {
        throw std::invalid_argument(
            "--autonomous-pulse-provider requires --openai-model MODEL");
    }
    if (options.openai_enabled && options.openai_model.empty()) {
        throw std::invalid_argument("OpenAI model must not be empty");
    }
    if (options.openai_enabled && options.openai_secret.empty()) {
        throw std::invalid_argument("OpenAI secret name must not be empty");
    }
    if (options.openai_enabled && options.secret_directory.empty()) {
        throw std::invalid_argument("secret directory must not be empty");
    }
    if (options.openai_enabled
        && (options.echo || options.enqueue_wait || options.enqueue_openai
            || options.inspect_task || options.cancel_task)) {
        throw std::invalid_argument(
            "OpenAI activation is only valid in service mode, with --check, or with --openai-once");
    }

    return options;
}

void require_distinct_regular_pulse_sidecar(const Options& options)
{
    if (options.autonomous_pulse_sidecar.empty()) return;

    const auto state_status = std::filesystem::symlink_status(options.state_path);
    const auto sidecar_status =
        std::filesystem::symlink_status(options.autonomous_pulse_sidecar);
    if (!std::filesystem::is_regular_file(state_status)) {
        throw std::invalid_argument(
            "autonomous pulse requires state database to be a regular non-symlink file");
    }
    if (!std::filesystem::is_regular_file(sidecar_status)) {
        throw std::invalid_argument(
            "autonomous pulse sidecar must already exist as a regular non-symlink file");
    }

    const auto state = std::filesystem::weakly_canonical(options.state_path);
    const auto sidecar =
        std::filesystem::weakly_canonical(options.autonomous_pulse_sidecar);
    if (state == sidecar) {
        throw std::invalid_argument(
            "autonomous pulse sidecar resolves to the state database");
    }
    std::error_code equivalent_error;
    if (std::filesystem::equivalent(options.state_path,
                                    options.autonomous_pulse_sidecar,
                                    equivalent_error)) {
        throw std::invalid_argument(
            "autonomous pulse sidecar aliases the state database");
    }
    if (equivalent_error) {
        throw std::runtime_error(
            "could not compare autonomous pulse sidecar identity");
    }

    const auto inspection = gaudere_agent::inspect_autonomous_cognition_pulse_sidecar(
        options.autonomous_pulse_sidecar);
    if (!inspection.eligible || !inspection.cursor) {
        throw std::invalid_argument(
            "autonomous pulse sidecar is not eligible: " + inspection.detail);
    }
}

void require_distinct_regular_local_activity_sidecar(const Options& options)
{
    if (options.local_activity_sidecar.empty()) return;

    const auto state_status = std::filesystem::symlink_status(options.state_path);
    const auto sidecar_status =
        std::filesystem::symlink_status(options.local_activity_sidecar);
    if (!std::filesystem::is_regular_file(state_status)) {
        throw std::invalid_argument(
            "local activity requires state database to be a regular non-symlink file");
    }
    if (!std::filesystem::is_regular_file(sidecar_status)) {
        throw std::invalid_argument(
            "local activity sidecar must already exist as a regular non-symlink file");
    }

    const auto state = std::filesystem::weakly_canonical(options.state_path);
    const auto sidecar = std::filesystem::weakly_canonical(options.local_activity_sidecar);
    if (state == sidecar) {
        throw std::invalid_argument(
            "local activity sidecar resolves to the state database");
    }
    std::error_code equivalent_error;
    if (std::filesystem::equivalent(options.state_path,
                                    options.local_activity_sidecar,
                                    equivalent_error)) {
        throw std::invalid_argument(
            "local activity sidecar aliases the state database");
    }
    if (equivalent_error) {
        throw std::runtime_error(
            "could not compare local activity sidecar identity");
    }

    const auto inspection = gaudere_agent::inspect_local_activity_pulse_sidecar(
        options.local_activity_sidecar);
    if (!inspection.eligible || !inspection.cursor) {
        throw std::invalid_argument(
            "local activity sidecar is not eligible: " + inspection.detail);
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

gaudere::work::Task make_echo_task(const Options& options)
{
    gaudere::work::Task task;
    task.id = options.task_id;
    task.idempotency_key = "local.echo:" + options.task_id;
    task.kind = "local.echo";
    task.input_content_type = "text/plain";
    task.input = options.text;
    task.limits.max_input_bytes = 4096;
    task.limits.max_output_bytes = 4096;
    task.limits.max_runtime = std::chrono::seconds{1};
    task.limits.max_attempts = 1;
    return task;
}

gaudere::work::Task make_wait_task(const Options& options)
{
    const auto duration = gaudere_agent::parse_local_wait_duration(options.text);
    if (!duration) {
        throw std::invalid_argument("invalid local.wait duration");
    }
    gaudere::work::Task task;
    task.id = options.task_id;
    task.idempotency_key = "local.wait:" + options.task_id;
    task.kind = "local.wait";
    task.input_content_type = "text/plain";
    task.input = options.text;
    task.limits.max_input_bytes = 32;
    task.limits.max_output_bytes = 64;
    task.limits.max_runtime = *duration + std::chrono::milliseconds{250};
    task.limits.max_attempts = 2;
    return task;
}

int inspect_task(gaudere::work::TaskStore& store, const std::string& id)
{
    const auto task = store.find(id);
    if (!task) {
        std::cerr << "gaudere-agent: task not found\n";
        return 3;
    }
    gaudere_agent::print_task_report(std::cout, *task);
    return 0;
}

int cancel_task(gaudere::work::Runtime& runtime,
                gaudere::work::TaskStore& store,
                const std::string& id,
                const std::string& reason)
{
    runtime.recover();
    if (!store.find(id)) {
        std::cerr << "gaudere-agent: task not found\n";
        return 3;
    }
    if (!runtime.request_cancel(id, reason)) {
        const auto task = store.find(id);
        if (task) {
            gaudere_agent::print_task_report(std::cout, *task);
        }
        std::cerr << "gaudere-agent: task is not cancellable\n";
        return 4;
    }
    const auto task = store.find(id);
    if (!task) {
        throw std::runtime_error("cancelled task disappeared from durable store");
    }
    gaudere_agent::print_task_report(std::cout, *task);
    return 0;
}

int enqueue_task(gaudere::work::Runtime& runtime,
                 gaudere::work::TaskStore& store,
                 gaudere::work::Task task,
                 const std::string& description)
{
    runtime.recover();
    const auto id = task.id;
    const auto submit = runtime.submit(task);
    if (submit != gaudere::work::SubmitResult::accepted
        && submit != gaudere::work::SubmitResult::duplicate) {
        throw std::runtime_error(description + " submission rejected");
    }
    const auto stored = store.find(id);
    if (!stored) {
        throw std::runtime_error(description + " task is missing after submission");
    }
    gaudere_agent::print_task_report(std::cout, *stored);
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        std::cout << std::unitbuf;
        require_distinct_regular_pulse_sidecar(options);
        require_distinct_regular_local_activity_sidecar(options);

        sigset_t signals{};
        if (!options.openai_once) {
            signals = block_control_signals();
        }

        gaudere_agent::StateLock state_lock(options.state_path);
        const auto now = [] { return std::chrono::system_clock::now(); };

        gaudere::persistence::sqlite::ActionStore action_store(options.state_path);
        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::scheduling::wake::Runtime action_runtime(action_store, now);
        gaudere::work::Runtime work_runtime(task_store, now);
        std::unique_ptr<gaudere::persistence::sqlite::WakeIntentStore>
            wake_intent_store;
        std::unique_ptr<gaudere::scheduling::wake::WakeIntentRuntime>
            wake_intent_runtime;
        std::unique_ptr<gaudere_agent::ExplicitWake> explicit_wake;
        if (options.wake_intents_enabled) {
            wake_intent_store =
                std::make_unique<gaudere::persistence::sqlite::WakeIntentStore>(
                    options.state_path);
            wake_intent_runtime =
                std::make_unique<gaudere::scheduling::wake::WakeIntentRuntime>(
                    *wake_intent_store, now, gaudere_agent::explicit_wake_scope,
                    gaudere::scheduling::wake::WakeIntentPolicy{
                        gaudere_agent::explicit_wake_max_total});
            explicit_wake = std::make_unique<gaudere_agent::ExplicitWake>(
                task_store, *wake_intent_runtime);
        }

        if (options.inspect_task) {
            return inspect_task(task_store, options.task_id);
        }
        if (options.cancel_task) {
            return cancel_task(work_runtime, task_store, options.task_id, options.text);
        }
        if (options.enqueue_wait) {
            return enqueue_task(work_runtime, task_store, make_wait_task(options),
                                "local.wait");
        }
        if (options.enqueue_openai) {
            return enqueue_task(
                work_runtime, task_store,
                gaudere_agent::make_openai_task(options.task_id, options.text),
                "OpenAI Responses");
        }

        gaudere::scheduling::wake::Scheduler work_scheduler;
        gaudere_agent::TaskExecutor task_executor(work_runtime, task_store);
        gaudere_agent::TaskDispatcher task_dispatcher(task_store, task_executor);
        gaudere_agent::LocalEchoHandler echo_handler;
        gaudere_agent::LocalWaitHandler wait_handler;
        gaudere_agent::WorkController work_controller(
            work_scheduler, work_runtime, task_dispatcher, "main-worker",
            wake_intent_runtime.get());
        std::unique_ptr<gaudere::persistence::sqlite::BudgetStore> provider_budget_store;
        std::unique_ptr<gaudere_agent::OpenAIActivation> openai_activation;
        std::unique_ptr<gaudere_agent::BoundedReflectionHandler> reflection_handler;
        std::unique_ptr<gaudere::persistence::sqlite::WakeIntentStore>
            pulse_wake_store;
        std::unique_ptr<gaudere_agent::AutonomousCognitionPulseStore>
            pulse_store;
        std::unique_ptr<gaudere_agent::AutonomousCognitionPulse> pulse;
        std::unique_ptr<gaudere_agent::AutonomousCognitionProviderGate>
            pulse_provider_gate;
        std::unique_ptr<gaudere_agent::AutonomousCognitionStaleRefresh>
            pulse_stale_refresh;
        std::unique_ptr<gaudere_agent::AutonomousCognitionPulseService>
            pulse_service;
        std::unique_ptr<gaudere_agent::OpenAIStructuredActivation>
            pulse_provider_activation;
        std::unique_ptr<gaudere_agent::CurrentCognitionHandler>
            pulse_cognition_handler;
        std::unique_ptr<gaudere_agent::AutonomousCognitionProviderService>
            pulse_provider_service;
        bool pulse_monitoring = false;
        std::unique_ptr<gaudere_agent::LocalActivityPulseStore>
            local_activity_store;
        std::unique_ptr<gaudere_agent::LocalActivityPulse>
            local_activity_pulse;
        std::unique_ptr<gaudere_agent::LocalActivityPulseService>
            local_activity_service;
        bool local_activity_monitoring = false;

        if (!task_dispatcher.register_handler("local.echo", echo_handler)
            || !task_dispatcher.register_handler("local.wait", wait_handler)) {
            throw std::runtime_error("cannot register local task handlers");
        }

        if (options.openai_enabled || !options.control_socket.empty()
            || !options.autonomous_pulse_sidecar.empty()
            || !options.local_activity_sidecar.empty()) {
            provider_budget_store =
                std::make_unique<gaudere::persistence::sqlite::BudgetStore>(
                    options.state_path);
        }

        if (options.openai_enabled) {
            openai_activation = std::make_unique<gaudere_agent::OpenAIActivation>(
                action_runtime, action_store, *provider_budget_store,
                options.openai_model, options.openai_secret,
                options.secret_directory);
            reflection_handler =
                std::make_unique<gaudere_agent::BoundedReflectionHandler>(
                    openai_activation->handler());
            if (!task_dispatcher.register_handler(
                    gaudere_agent::openai_task_kind, openai_activation->handler())
                || !task_dispatcher.register_handler(
                    gaudere_agent::bounded_reflection_task_kind,
                    *reflection_handler)) {
                throw std::runtime_error("cannot register OpenAI provider handler");
            }
            std::cout << "gaudere-agent: OpenAI provider enabled model="
                      << options.openai_model << " secret="
                      << options.openai_secret << '\n';
            const auto& budget = openai_activation->budget_policy();
            std::cout << "gaudere-agent: bounded reflection enabled kind="
                      << gaudere_agent::bounded_reflection_task_kind
                      << " automatic_scheduling=false\n";
            std::cout << "gaudere-agent: OpenAI budget max_total="
                      << budget.max_total << " max_window="
                      << budget.max_in_window << " window_seconds="
                      << std::chrono::duration_cast<std::chrono::seconds>(
                             budget.window).count()
                      << " min_interval_seconds="
                      << std::chrono::duration_cast<std::chrono::seconds>(
                             budget.min_interval).count()
                      << '\n';
        }

        if (options.wake_intents_enabled) {
            std::cout << "gaudere-agent: explicit wake enabled scope="
                      << gaudere_agent::explicit_wake_scope
                      << " max_total=" << gaudere_agent::explicit_wake_max_total
                      << " automatic_successor=false\n";
        }

        if (!options.autonomous_pulse_sidecar.empty()) {
            if (!provider_budget_store) {
                throw std::runtime_error(
                    "autonomous pulse provider budget reader is unavailable");
            }
            gaudere::scheduling::wake::WakeIntentStore* pulse_wakes =
                wake_intent_store.get();
            if (!pulse_wakes) {
                pulse_wake_store =
                    std::make_unique<gaudere::persistence::sqlite::WakeIntentStore>(
                        options.state_path);
                pulse_wakes = pulse_wake_store.get();
            }
            pulse_store =
                std::make_unique<gaudere_agent::AutonomousCognitionPulseStore>(
                    options.autonomous_pulse_sidecar);
            if (!pulse_store->find(gaudere_agent::autonomous_cognition_pulse_scope)) {
                throw std::runtime_error(
                    "autonomous pulse sidecar lost its seeded cursor after inspection");
            }
            pulse = std::make_unique<gaudere_agent::AutonomousCognitionPulse>(
                *pulse_store, task_store, *provider_budget_store, *pulse_wakes,
                work_runtime, now, true);
            pulse_provider_gate =
                std::make_unique<gaudere_agent::AutonomousCognitionProviderGate>(
                    options.state_path, task_store, *provider_budget_store,
                    action_store, now);
            pulse_stale_refresh =
                std::make_unique<gaudere_agent::AutonomousCognitionStaleRefresh>(
                    *pulse_store, task_store, action_store, work_runtime,
                    *pulse_provider_gate);
            pulse_service =
                std::make_unique<gaudere_agent::AutonomousCognitionPulseService>(
                    *pulse, *provider_budget_store, work_scheduler, now,
                    pulse_stale_refresh.get());

            if (options.autonomous_pulse_provider) {
                pulse_provider_activation =
                    std::make_unique<gaudere_agent::OpenAIStructuredActivation>(
                        action_runtime, action_store, *provider_budget_store,
                        gaudere_agent::resume_decision_structured_output_contract(),
                        options.openai_model, options.openai_secret,
                        options.secret_directory);
                pulse_cognition_handler =
                    std::make_unique<gaudere_agent::CurrentCognitionHandler>(
                        pulse_provider_activation->handler());
                pulse_provider_service =
                    std::make_unique<gaudere_agent::AutonomousCognitionProviderService>(
                        *pulse_service, *pulse_provider_gate, task_executor,
                        *pulse_cognition_handler, task_store, work_scheduler, now);
                std::cout
                    << "gaudere-agent: autonomous cognition pulse enabled sidecar="
                    << options.autonomous_pulse_sidecar
                    << " provider_execution=true automatic_seed=false\n";
            } else {
                std::cout
                    << "gaudere-agent: autonomous cognition pulse enabled sidecar="
                    << options.autonomous_pulse_sidecar
                    << " provider_execution=false automatic_seed=false\n";
            }
        }

        if (!options.local_activity_sidecar.empty()) {
            if (!provider_budget_store) {
                throw std::runtime_error(
                    "local activity provider budget reader is unavailable");
            }
            gaudere::scheduling::wake::WakeIntentStore* local_wakes =
                wake_intent_store.get();
            if (!local_wakes) {
                if (!pulse_wake_store) {
                    pulse_wake_store =
                        std::make_unique<gaudere::persistence::sqlite::WakeIntentStore>(
                            options.state_path);
                }
                local_wakes = pulse_wake_store.get();
            }
            local_activity_store =
                std::make_unique<gaudere_agent::LocalActivityPulseStore>(
                    options.local_activity_sidecar);
            if (!local_activity_store->find(gaudere_agent::local_activity_pulse_scope)) {
                throw std::runtime_error(
                    "local activity sidecar lost its seeded cursor after inspection");
            }
            local_activity_pulse =
                std::make_unique<gaudere_agent::LocalActivityPulse>(
                    *local_activity_store, task_store, action_store,
                    *provider_budget_store, *local_wakes, work_runtime, now, true);
            local_activity_service =
                std::make_unique<gaudere_agent::LocalActivityPulseService>(
                    *local_activity_pulse, *local_activity_store, work_scheduler);
            std::cout
                << "gaudere-agent: local continuity activity enabled sidecar="
                << options.local_activity_sidecar
                << " provider_execution=false automatic_seed=false max_generations=3\n";
        }

        action_runtime.recover();
        work_runtime.recover();
        if (!work_controller.start()) {
            throw std::runtime_error("cannot start work controller");
        }

        const auto step_autonomous_pulse = [&]() {
            if (!pulse_service) return true;
            if (pulse_provider_service) {
                const auto step = pulse_provider_service->step();
                pulse_monitoring = step.healthy && step.monitoring;
                if (step.provider_executed && step.task_id) {
                    std::cout << "gaudere-agent: autonomous provider executed task="
                              << *step.task_id << '\n';
                }
                if (!step.detail.empty()) {
                    std::cout << "gaudere-agent: autonomous pulse provider: "
                              << step.detail << '\n';
                }
                if (!step.healthy) {
                    std::cerr
                        << "gaudere-agent: autonomous pulse provider monitoring disabled\n";
                }
                return step.healthy;
            }

            const auto step = pulse_service->step();
            pulse_monitoring = step.plan.healthy && step.plan.monitoring;
            if (step.observation.task) {
                std::cout << "gaudere-agent: autonomous pulse task="
                          << step.observation.task->id
                          << " provider_execution=false\n";
            }
            if (!step.plan.detail.empty()) {
                std::cout << "gaudere-agent: autonomous pulse: "
                          << step.plan.detail << '\n';
            }
            if (!step.plan.healthy) {
                std::cerr
                    << "gaudere-agent: autonomous pulse monitoring disabled\n";
            }
            return step.plan.healthy;
        };

        const auto step_local_activity = [&]() {
            if (!local_activity_service) return true;
            const auto step = local_activity_service->step();
            local_activity_monitoring = step.healthy && step.monitoring;
            if (step.observation.task) {
                std::cout << "gaudere-agent: local continuity activity task="
                          << step.observation.task->id
                          << " provider_execution=false\n";
            }
            if (!step.detail.empty()) {
                std::cout << "gaudere-agent: local continuity activity: "
                          << step.detail << '\n';
            }
            if (!step.healthy) {
                std::cerr
                    << "gaudere-agent: local continuity activity monitoring disabled\n";
            }
            if (step.healthy && !step.monitoring
                && step.observation.result
                    == gaudere_agent::LocalActivityPulseResult::quiescent) {
                std::cout
                    << "gaudere-agent: local continuity activity quiescent after generation 3\n";
            }
            return step.healthy;
        };

        if (pulse_service && !step_autonomous_pulse()) {
            throw std::runtime_error("autonomous pulse startup failed");
        }
        if (local_activity_service && !step_local_activity()) {
            throw std::runtime_error("local continuity activity startup failed");
        }

        if (options.echo) {
            std::cout << "gaudere-agent: running\n";
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
            const auto stored = task_store.find(options.task_id);
            if (!stored || stored->status != gaudere::work::TaskStatus::succeeded
                || !stored->result) {
                throw std::runtime_error("local.echo did not complete successfully");
            }
            std::cout << "gaudere-agent: echo result: " << stored->result->output << '\n';
            work_controller.stop();
            static_cast<void>(work_controller.wait_and_run());
        } else if (options.openai_once) {
            std::cout << "gaudere-agent: running\n";
            gaudere_agent::run_openai_once(
                work_runtime, task_store, work_controller,
                options.task_id, options.text);
        } else {
            std::unique_ptr<gaudere_agent::LiveControlMailbox> control_mailbox;
            std::unique_ptr<gaudere_agent::LiveControlProcessor> control_processor;
            std::unique_ptr<gaudere_agent::LiveControlServer> control_server;
            if (!options.control_socket.empty()) {
                if (!provider_budget_store) {
                    throw std::runtime_error(
                        "live control provider budget store is unavailable");
                }
                control_mailbox = std::make_unique<gaudere_agent::LiveControlMailbox>();
                control_processor = std::make_unique<gaudere_agent::LiveControlProcessor>(
                    work_runtime, task_store, *provider_budget_store,
                    gaudere_agent::OpenAIActivation::bootstrap_budget_policy(),
                    options.openai_enabled, explicit_wake.get(),
                    [&work_scheduler] { return work_scheduler.next(); });
                control_server = std::make_unique<gaudere_agent::LiveControlServer>(
                    options.control_socket, *control_mailbox,
                    [&work_controller] { work_controller.interrupt(); });
                if (!control_server->start()) {
                    throw std::runtime_error("cannot start live control server");
                }
                std::cout << "gaudere-agent: control socket="
                          << options.control_socket << '\n';
            }

            std::cout << "gaudere-agent: running\n";

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
                if (control_processor) {
                    const auto control = control_processor->process(*control_mailbox);
                    if (control.work_may_be_pending) {
                        work_controller.notify_work();
                    }
                    if (control.wake_deadline_may_have_changed) {
                        work_controller.refresh_deadlines();
                    }
                }

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
                    continue;
                }

                if (pulse_service && pulse_monitoring) {
                    static_cast<void>(step_autonomous_pulse());
                }
                if (local_activity_service && local_activity_monitoring) {
                    static_cast<void>(step_local_activity());
                }
            }

            if (control_server) {
                control_server->stop();
                control_server->join();
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
