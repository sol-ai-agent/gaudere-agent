#include "LiveControl.hpp"
#include "LiveControlProcessor.hpp"
#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueThreadStore.hpp"
#include "LocalGooseDialogueV3.hpp"
#include "LocalGooseDialogueV3Handler.hpp"
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

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string state_path;
    std::string model;
    std::string model_sha256;
    std::string control_socket;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string thread_alias = "main";
    std::string goose_path_root = "/var/lib/gaudere/goose";
    std::size_t expected_tasks = 0;
    std::chrono::milliseconds linger{0};
};

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " --state PATH --model SELECTOR --model-sha256 SHA256"
        << " --control-socket PATH --thread-sidecar PATH"
        << " --completion-sidecar PATH --expected-tasks N"
        << " [--linger-ms N]"
        << " [--thread-alias ALIAS] [--goose-root PATH]\n";
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

bool safe_alias(const std::string& value)
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool absolute_path(const std::string& value)
{
    return !value.empty() && value.front() == '/';
}

std::size_t parse_size(const std::string& value)
{
    if (value.empty()) {
        throw std::invalid_argument("empty expected task count");
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size() || parsed == 0 || parsed > 16) {
        throw std::invalid_argument(
            "expected task count must be an integer in 1..16");
    }
    return static_cast<std::size_t>(parsed);
}

std::chrono::milliseconds parse_milliseconds(const std::string& value)
{
    if (value.empty()) {
        throw std::invalid_argument("empty linger duration");
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed, 10);
    if (consumed != value.size() || parsed < 0 || parsed > 60000) {
        throw std::invalid_argument(
            "linger duration must be an integer in 0..60000 ms");
    }
    return std::chrono::milliseconds{parsed};
}

Options parse_options(const int argc, char* argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--state" && i + 1 < argc) {
            options.state_path = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            options.model = argv[++i];
        } else if (arg == "--model-sha256" && i + 1 < argc) {
            options.model_sha256 = argv[++i];
        } else if (arg == "--control-socket" && i + 1 < argc) {
            options.control_socket = argv[++i];
        } else if (arg == "--thread-sidecar" && i + 1 < argc) {
            options.thread_sidecar = argv[++i];
        } else if (arg == "--completion-sidecar" && i + 1 < argc) {
            options.completion_sidecar = argv[++i];
        } else if (arg == "--thread-alias" && i + 1 < argc) {
            options.thread_alias = argv[++i];
        } else if (arg == "--expected-tasks" && i + 1 < argc) {
            options.expected_tasks = parse_size(argv[++i]);
        } else if (arg == "--linger-ms" && i + 1 < argc) {
            options.linger = parse_milliseconds(argv[++i]);
        } else if (arg == "--goose-root" && i + 1 < argc) {
            options.goose_path_root = argv[++i];
        } else if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown or incomplete argument: " + arg);
        }
    }

    if (options.state_path.empty()
        || options.model.empty()
        || options.model_sha256.empty()
        || options.control_socket.empty()
        || options.thread_sidecar.empty()
        || options.completion_sidecar.empty()
        || options.expected_tasks == 0
        || options.goose_path_root.empty()) {
        throw std::invalid_argument(
            "dialogue v3 proof runtime options are incomplete");
    }
    if (!absolute_path(options.state_path)
        || !absolute_path(options.model)
        || !absolute_path(options.control_socket)
        || !absolute_path(options.thread_sidecar)
        || !absolute_path(options.completion_sidecar)
        || !absolute_path(options.goose_path_root)) {
        throw std::invalid_argument(
            "dialogue v3 proof paths/model must use absolute form");
    }
    if (!canonical_sha256(options.model_sha256)) {
        throw std::invalid_argument(
            "dialogue v3 proof model SHA256 must be canonical lowercase hex");
    }
    if (!safe_alias(options.thread_alias)) {
        throw std::invalid_argument("dialogue v3 proof thread alias is invalid");
    }
    return options;
}

void validate_existing_regular(const std::string& path, const char* label)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            std::string{"dialogue v3 proof "} + label
            + " must be an existing regular non-symlink file");
    }
}

void validate_optional_sidecar(const std::string& path, const char* label)
{
    if (!std::filesystem::exists(path)) return;
    validate_existing_regular(path, label);
}

void validate_paths(const Options& options)
{
    validate_existing_regular(options.state_path, "state");
    const auto goose = std::filesystem::symlink_status(options.goose_path_root);
    if (!std::filesystem::is_directory(goose)) {
        throw std::invalid_argument(
            "dialogue v3 proof Goose root must be an existing directory");
    }
    if (std::filesystem::exists(options.control_socket)) {
        throw std::invalid_argument(
            "dialogue v3 proof control socket path must not preexist");
    }
    validate_optional_sidecar(options.thread_sidecar, "thread sidecar");
    validate_optional_sidecar(options.completion_sidecar, "completion sidecar");
}

std::int64_t epoch_milliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        validate_paths(options);
        std::cout << std::unitbuf;

        gaudere_agent::StateLock state_lock(options.state_path);
        const auto now = [] { return std::chrono::system_clock::now(); };

        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        gaudere::persistence::sqlite::BudgetStore budget_store(options.state_path);
        gaudere::work::Runtime runtime(task_store, now);
        gaudere::scheduling::wake::Scheduler scheduler;
        gaudere_agent::TaskExecutor executor(runtime, task_store);
        gaudere_agent::TaskDispatcher dispatcher(task_store, executor);
        gaudere_agent::WorkController controller(
            scheduler, runtime, dispatcher, "local-goose-dialogue-v3-proof");

        gaudere_agent::LocalGooseDialogueThreadStore thread_store(
            options.thread_sidecar);
        gaudere_agent::LocalGooseDialogueCompletionFeedStore completion_store(
            options.completion_sidecar);
        gaudere_agent::LocalGooseDialogueCompletionFeedService completion_service(
            thread_store,
            completion_store,
            [&task_store](const std::string& id)
                -> std::optional<gaudere::work::Task> {
                return task_store.find(id);
            },
            [] { return epoch_milliseconds(); });

        gaudere_agent::PosixLocalGooseRunner runner;
        gaudere_agent::LocalGooseDialogueV3Handler dialogue_handler(
            runner,
            [&task_store](const std::string& id)
                -> std::optional<gaudere::work::Task> {
                return task_store.find(id);
            },
            options.model,
            options.model_sha256,
            options.goose_path_root);
        if (!dispatcher.register_handler(
                gaudere_agent::local_goose_dialogue_v3_task_kind,
                dialogue_handler)) {
            throw std::runtime_error(
                "cannot register Local Goose dialogue v3 proof handler");
        }

        gaudere_agent::LiveControlMailbox mailbox;
        gaudere_agent::LiveControlProcessor processor(
            runtime,
            task_store,
            budget_store,
            gaudere_agent::openai_bootstrap_budget_policy(),
            false,
            nullptr,
            {},
            {},
            options.model_sha256,
            &thread_store);
        gaudere_agent::LiveControlServer server(
            options.control_socket,
            mailbox,
            [&controller] { controller.interrupt(); });

        const auto reconcile_feed = [&] {
            if (!thread_store.find(options.thread_alias)) return;
            const auto result =
                completion_service.reconcile(options.thread_alias, 64);
            if (result.blocked) {
                throw std::runtime_error(
                    "dialogue v3 proof completion feed blocked: "
                    + result.detail);
            }
            if (result.materialized != 0) {
                std::cout
                    << "gaudere-local-goose-dialogue-v3-proof:"
                    << " completion_materialized=" << result.materialized
                    << "\n";
            }
        };

        runtime.recover();
        if (!controller.start()) {
            throw std::runtime_error(
                "cannot start Local Goose dialogue v3 proof work controller");
        }
        if (!server.start()) {
            throw std::runtime_error(
                "cannot start Local Goose dialogue v3 proof control server");
        }

        reconcile_feed();
        std::cout
            << "gaudere-local-goose-dialogue-v3-proof: running"
            << " provider_execution=false tools_enabled=false"
            << " network_expected=none multi_actor=true"
            << " preferred_head_cas=true completion_feed=true"
            << " expected_tasks=" << options.expected_tasks
            << " linger_ms=" << options.linger.count() << "\n";

        std::size_t worked_tasks = 0;
        bool conflict = false;
        while (worked_tasks < options.expected_tasks && !conflict) {
            const auto control = processor.process(mailbox);
            if (control.work_may_be_pending) controller.notify_work();
            if (control.wake_deadline_may_have_changed)
                controller.refresh_deadlines();

            reconcile_feed();

            const auto work = controller.wait_and_run();
            if (work == gaudere_agent::WorkCycleResult::worked) {
                ++worked_tasks;
                reconcile_feed();
            } else if (work == gaudere_agent::WorkCycleResult::state_conflict) {
                conflict = true;
            } else if (work == gaudere_agent::WorkCycleResult::stopped) {
                break;
            }
        }

        if (!conflict && worked_tasks == options.expected_tasks
            && options.linger > std::chrono::milliseconds::zero()) {
            const auto until = std::chrono::steady_clock::now() + options.linger;
            while (std::chrono::steady_clock::now() < until) {
                const auto control = processor.process(mailbox);
                if (control.work_may_be_pending) {
                    throw std::runtime_error(
                        "dialogue v3 proof received an unexpected submission during linger");
                }
                if (control.wake_deadline_may_have_changed
                    || control.local_goose_cycle_may_have_changed) {
                    throw std::runtime_error(
                        "dialogue v3 proof received unexpected authority during linger");
                }
                reconcile_feed();
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
            }
        }

        server.stop();
        server.join();

        if (conflict || worked_tasks != options.expected_tasks) {
            std::cerr
                << "gaudere-local-goose-dialogue-v3-proof: expected "
                << options.expected_tasks << " dialogue Tasks, completed "
                << worked_tasks << "\n";
            return 2;
        }

        reconcile_feed();
        const auto head = thread_store.find(options.thread_alias);
        if (!head) {
            std::cerr
                << "gaudere-local-goose-dialogue-v3-proof: preferred thread is missing\n";
            return 2;
        }
        std::cout << gaudere_agent::local_goose_dialogue_thread_head_report(*head);

        runtime.request_shutdown();
        if (!runtime.try_mark_safe()) {
            std::cerr
                << "gaudere-local-goose-dialogue-v3-proof: runtime is not safe to stop\n";
            return 2;
        }

        std::cout << "FEDORA_LOCAL_GOOSE_DIALOGUE_V3_RUNTIME=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere-local-goose-dialogue-v3-proof: "
            << error.what() << '\n';
        return 1;
    }
}
