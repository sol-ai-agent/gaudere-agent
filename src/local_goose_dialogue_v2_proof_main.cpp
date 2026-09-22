#include "LiveControl.hpp"
#include "LiveControlProcessor.hpp"
#include "LocalGooseDialogueV2.hpp"
#include "LocalGooseDialogueV2Handler.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string state_path;
    std::string model;
    std::string model_sha256;
    std::string control_socket;
    std::string goose_path_root = "/var/lib/gaudere/goose";
};

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " --state PATH --model SELECTOR --model-sha256 SHA256"
        << " --control-socket PATH [--goose-root PATH]\n";
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

bool absolute_path(const std::string& value)
{
    return !value.empty() && value.front() == '/';
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
        || options.goose_path_root.empty()) {
        throw std::invalid_argument("dialogue v2 proof runtime options are incomplete");
    }
    if (!absolute_path(options.state_path)
        || !absolute_path(options.model)
        || !absolute_path(options.control_socket)
        || !absolute_path(options.goose_path_root)) {
        throw std::invalid_argument(
            "dialogue v2 proof state/model/socket/goose-root must use absolute form");
    }
    if (!canonical_sha256(options.model_sha256)) {
        throw std::invalid_argument(
            "dialogue v2 proof model SHA256 must be canonical lowercase hex");
    }
    return options;
}

void validate_paths(const Options& options)
{
    const auto state = std::filesystem::symlink_status(options.state_path);
    if (!std::filesystem::is_regular_file(state)) {
        throw std::invalid_argument(
            "dialogue v2 proof state must be an existing regular non-symlink file");
    }
    const auto goose = std::filesystem::symlink_status(options.goose_path_root);
    if (!std::filesystem::is_directory(goose)) {
        throw std::invalid_argument(
            "dialogue v2 proof Goose root must be an existing directory");
    }
    if (std::filesystem::exists(options.control_socket)) {
        throw std::invalid_argument(
            "dialogue v2 proof control socket path must not preexist");
    }
}

gaudere::work::Task succeeded_v2_task(
    gaudere::work::Task task,
    const std::string& response)
{
    const auto inspection =
        gaudere_agent::inspect_local_goose_dialogue_v2_task(task);
    if (!inspection.eligible) {
        throw std::runtime_error(
            "dialogue v2 context-bound self-check built an invalid Task");
    }
    task.status = gaudere::work::TaskStatus::succeeded;
    task.attempts_started = 1;
    task.result = gaudere::work::TaskResult{
        gaudere_agent::local_goose_dialogue_v2_response_content_type,
        gaudere_agent::make_local_goose_dialogue_v2_response(
            inspection, response),
        {},
        {}};
    return task;
}

gaudere_agent::LocalGooseDialogueV2History resolve_synthetic_history(
    const gaudere::work::Task& current,
    const std::map<std::string, gaudere::work::Task>& tasks)
{
    return gaudere_agent::resolve_local_goose_dialogue_v2_history(
        current,
        [&tasks](const std::string& id)
            -> std::optional<gaudere::work::Task> {
            const auto it = tasks.find(id);
            if (it == tasks.end()) return std::nullopt;
            return it->second;
        });
}

void prove_context_bounds(const std::string& model_sha256)
{
    {
        std::map<std::string, gaudere::work::Task> tasks;
        auto current = succeeded_v2_task(
            gaudere_agent::make_local_goose_dialogue_v2_root_task(
                "v2-proof-small-0", "small root", model_sha256),
            "ok");
        tasks.emplace(current.id, current);

        for (int i = 1; i <= 8; ++i) {
            auto next =
                gaudere_agent::make_local_goose_dialogue_v2_successor_task(
                    "v2-proof-small-" + std::to_string(i),
                    "small turn " + std::to_string(i),
                    model_sha256,
                    current);
            current = succeeded_v2_task(std::move(next), "ok");
            tasks[current.id] = current;
        }

        const auto history = resolve_synthetic_history(current, tasks);
        if (!history.eligible
            || !history.truncated
            || history.turns.size() != 6
            || history.turns.front().turn_index != 2
            || history.turns.back().turn_index != 7) {
            throw std::runtime_error(
                "dialogue v2 six-predecessor context bound self-check failed");
        }
    }

    {
        std::map<std::string, gaudere::work::Task> tasks;
        const std::string large_response(7000, 'x');
        auto current = succeeded_v2_task(
            gaudere_agent::make_local_goose_dialogue_v2_root_task(
                "v2-proof-large-0", "large root", model_sha256),
            large_response);
        tasks.emplace(current.id, current);

        for (int i = 1; i <= 8; ++i) {
            auto next =
                gaudere_agent::make_local_goose_dialogue_v2_successor_task(
                    "v2-proof-large-" + std::to_string(i),
                    "large turn " + std::to_string(i),
                    model_sha256,
                    current);
            current = succeeded_v2_task(std::move(next), large_response);
            tasks[current.id] = current;
        }

        const auto history = resolve_synthetic_history(current, tasks);
        if (!history.eligible
            || !history.truncated
            || history.turns.empty()
            || history.turns.size() >= 6) {
            throw std::runtime_error(
                "dialogue v2 40 KiB history context bound self-check failed");
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        validate_paths(options);
        prove_context_bounds(options.model_sha256);
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
            scheduler, runtime, dispatcher, "local-goose-dialogue-v2-proof");

        gaudere_agent::PosixLocalGooseRunner runner;
        gaudere_agent::LocalGooseDialogueV2Handler dialogue_handler(
            runner,
            [&task_store](const std::string& id)
                -> std::optional<gaudere::work::Task> {
                return task_store.find(id);
            },
            options.model,
            options.model_sha256,
            options.goose_path_root);
        if (!dispatcher.register_handler(
                gaudere_agent::local_goose_dialogue_v2_task_kind,
                dialogue_handler)) {
            throw std::runtime_error(
                "cannot register Local Goose dialogue v2 proof handler");
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
            options.model_sha256);
        gaudere_agent::LiveControlServer server(
            options.control_socket,
            mailbox,
            [&controller] { controller.interrupt(); });

        runtime.recover();
        if (!controller.start()) {
            throw std::runtime_error(
                "cannot start Local Goose dialogue v2 proof work controller");
        }
        if (!server.start()) {
            throw std::runtime_error(
                "cannot start Local Goose dialogue v2 proof control server");
        }

        std::cout
            << "gaudere-local-goose-dialogue-v2-proof: running"
            << " provider_execution=false tools_enabled=false network_expected=none"
            << " context_bound_self_check=pass"
            << " max_history_turns=6 max_history_bytes=40960"
            << " max_prompt_bytes=49152\n";

        constexpr std::size_t expected_worked_tasks = 4;
        std::size_t worked_tasks = 0;
        bool conflict = false;
        while (worked_tasks < expected_worked_tasks && !conflict) {
            const auto control = processor.process(mailbox);
            if (control.work_may_be_pending) controller.notify_work();
            if (control.wake_deadline_may_have_changed)
                controller.refresh_deadlines();

            const auto work = controller.wait_and_run();
            if (work == gaudere_agent::WorkCycleResult::worked) {
                ++worked_tasks;
            } else if (work == gaudere_agent::WorkCycleResult::state_conflict) {
                conflict = true;
            } else if (work == gaudere_agent::WorkCycleResult::stopped) {
                break;
            }
        }

        server.stop();
        server.join();

        if (conflict || worked_tasks != expected_worked_tasks) {
            std::cerr
                << "gaudere-local-goose-dialogue-v2-proof: expected four dialogue Tasks, completed "
                << worked_tasks << "\n";
            return 2;
        }

        runtime.request_shutdown();
        if (!runtime.try_mark_safe()) {
            std::cerr
                << "gaudere-local-goose-dialogue-v2-proof: runtime is not safe to stop\n";
            return 2;
        }

        std::cout << "FEDORA_LOCAL_GOOSE_DIALOGUE_V2_RUNTIME=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere-local-goose-dialogue-v2-proof: "
            << error.what() << '\n';
        return 1;
    }
}
