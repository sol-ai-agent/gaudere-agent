#include "LocalGooseRunner.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

int main()
{
    const std::string goose_root = "/tmp/gaudere-local-goose-tools-root";
    std::filesystem::remove_all(goose_root);
    std::filesystem::create_directories(goose_root);

    gaudere_agent::LocalGooseRunRequest request;
    request.model_id = "unsloth/gemma-4-E4B-it-GGUF:Q4_K_M";
    request.goose_path_root = goose_root;
    request.prompt = "Use Gaudere's own typed tools when useful.";
    request.tools_enabled = true;
    request.control_socket = "/tmp/gaudere-control.sock";
    request.governance_path = "/var/lib/gaudere/goose-governance.db";

    const auto invocation = gaudere_agent::make_goose_cli_invocation(request);
    assert(invocation.binary == "/usr/local/bin/goose");
    assert(contains(invocation.argv, "--with-extension"));
    assert(contains(invocation.environment, "GOOSE_MODE=auto"));
    assert(contains(invocation.environment, "GOOSE_MAX_TURNS=32"));
    assert(contains(invocation.environment, "GOOSE_MODEL=unsloth/gemma-4-E4B-it-GGUF:Q4_K_M"));
    assert(contains(invocation.environment, "GOOSE_PATH_ROOT=" + goose_root));
    assert(!contains(invocation.environment, "GOOSE_MODE=chat"));
    assert(!contains(invocation.argv, "/bin/sh"));
    assert(!contains(invocation.argv, "/bin/bash"));

    const auto extension = std::find(invocation.argv.begin(), invocation.argv.end(),
                                     "--with-extension");
    assert(extension != invocation.argv.end());
    assert(extension + 1 != invocation.argv.end());
    assert(*(extension + 1)
           == "/usr/local/bin/gaudere-goose-tools-mcp --socket /tmp/gaudere-control.sock --governance /var/lib/gaudere/goose-governance.db");

    request.control_socket = "/tmp/unsafe socket";
    bool rejected = false;
    try {
        static_cast<void>(gaudere_agent::make_goose_cli_invocation(request));
    } catch (...) {
        rejected = true;
    }
    assert(rejected);

    std::filesystem::remove_all(goose_root);
    std::cout << "local Goose typed-tool invocation tests passed\n";
    return 0;
}
