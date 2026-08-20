#include "LiveControl.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program << " --socket PATH "
        << "[echo ID TEXT | openai ID TEXT | task ID | budget]\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc < 4 || std::string(argv[1]) != "--socket") {
            usage(argv[0]);
            return 2;
        }
        const std::string socket_path = argv[2];
        const std::string operation = argv[3];

        gaudere_agent::LiveControlCommand command;
        if (operation == "echo" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::submit_echo;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "openai" && argc == 6) {
            command.operation = gaudere_agent::LiveControlOperation::submit_openai;
            command.id = argv[4];
            command.text = argv[5];
        } else if (operation == "task" && argc == 5) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_task;
            command.id = argv[4];
        } else if (operation == "budget" && argc == 4) {
            command.operation = gaudere_agent::LiveControlOperation::inspect_budget;
            command.id = "openai";
        } else {
            usage(argv[0]);
            return 2;
        }

        return gaudere_agent::run_live_control_client(
            socket_path, command, std::cout, std::cerr);
    } catch (const std::exception& error) {
        std::cerr << "gaudere-control: " << error.what() << '\n';
        return 1;
    }
}
