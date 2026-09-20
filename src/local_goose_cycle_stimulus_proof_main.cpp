#include "LiveControl.hpp"
#include "LocalGooseCycleStimulusControl.hpp"
#include "LocalGooseCycleStimulusService.hpp"
#include "LocalGooseCycleStimulusStore.hpp"
#include "LocalGooseCycleStore.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string state_path;
    std::string cycle_sidecar;
    std::string stimulus_sidecar;
    std::string control_socket;
};

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " --state PATH --cycle-sidecar PATH"
        << " --stimulus-sidecar PATH --control-socket PATH\n";
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
        } else if (argument == "--control-socket" && index + 1 < argc) {
            options.control_socket = argv[++index];
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument(
                "unknown or incomplete argument: " + argument);
        }
    }

    if (options.state_path.empty() || options.cycle_sidecar.empty()
        || options.stimulus_sidecar.empty()
        || options.control_socket.empty()) {
        throw std::invalid_argument("all proof paths are required");
    }
    for (const auto* path : {
             &options.state_path,
             &options.cycle_sidecar,
             &options.stimulus_sidecar,
             &options.control_socket}) {
        if (path->front() != '/') {
            throw std::invalid_argument(
                "proof paths must use absolute form");
        }
    }
    return options;
}

void require_regular_non_symlink(
    const std::string& path,
    const std::string& description)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            description + " must be a regular non-symlink file");
    }
}

void require_distinct(
    const std::string& left,
    const std::string& right,
    const std::string& description)
{
    const auto left_path = std::filesystem::weakly_canonical(left);
    const auto right_path = std::filesystem::weakly_canonical(right);
    if (left_path == right_path) {
        throw std::invalid_argument(description);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);

        require_regular_non_symlink(
            options.state_path, "proof state database");
        require_regular_non_symlink(
            options.cycle_sidecar, "proof Local Goose cycle sidecar");
        if (std::filesystem::exists(options.stimulus_sidecar)) {
            throw std::invalid_argument(
                "proof stimulus sidecar must not pre-exist");
        }
        if (std::filesystem::exists(options.control_socket)) {
            throw std::invalid_argument(
                "proof control socket must not pre-exist");
        }
        require_distinct(
            options.state_path, options.cycle_sidecar,
            "proof state and cycle sidecar must be distinct");
        require_distinct(
            options.state_path, options.stimulus_sidecar,
            "proof state and stimulus sidecar must be distinct");
        require_distinct(
            options.cycle_sidecar, options.stimulus_sidecar,
            "proof cycle and stimulus sidecars must be distinct");

        gaudere::persistence::sqlite::TaskStore task_store(
            options.state_path);
        gaudere_agent::LocalGooseCycleStore cycle_store(
            options.cycle_sidecar);
        gaudere_agent::LocalGooseCycleStimulusStore stimulus_store(
            options.stimulus_sidecar);
        gaudere_agent::LocalGooseCycleStimulusService stimulus_service(
            stimulus_store, cycle_store, task_store);
        gaudere_agent::LocalGooseCycleStimulusControl stimulus_control(
            stimulus_service,
            [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            });

        gaudere_agent::LiveControlMailbox mailbox;
        std::mutex mutex;
        std::condition_variable condition;
        bool wake_pending = false;

        gaudere_agent::LiveControlServer server(
            options.control_socket, mailbox,
            [&] {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    wake_pending = true;
                }
                condition.notify_one();
            });

        if (!server.start()) {
            throw std::runtime_error(
                "proof LiveControl server did not start");
        }

        std::cout
            << "gaudere-local-goose-cycle-stimulus-proof: ready"
            << " provider_execution=false"
            << " cycle_execution=false"
            << " network_authority=false\n"
            << std::flush;

        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return wake_pending; });
        lock.unlock();

        auto pending = mailbox.take_all();
        if (pending.size() != 1) {
            throw std::runtime_error(
                "proof runtime requires exactly one live-control request");
        }

        const auto& command = pending.front()->command();
        gaudere_agent::LiveControlReply reply;
        if (command.operation
            != gaudere_agent::LiveControlOperation::
                stimulate_local_goose_cycle) {
            reply = {
                false, 4,
                "gaudere-agent: proof runtime accepts only "
                "stimulate_local_goose_cycle\n"};
        } else {
            reply = stimulus_control.stimulate(command.id);
        }

        pending.front()->complete(reply);
        server.stop();
        server.join();

        std::cout << reply.body << std::flush;
        if (!reply.ok) {
            return reply.code == 0 ? 4 : reply.code;
        }

        std::cout
            << "FEDORA_LOCAL_GOOSE_STIMULUS_CONTROL=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere-local-goose-cycle-stimulus-proof: "
            << error.what() << '\n';
        return 1;
    }
}
