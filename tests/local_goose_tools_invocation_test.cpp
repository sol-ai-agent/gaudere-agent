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
    assert(contains(invocation.argv, "--quiet"));
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

    const auto output_format = std::find(invocation.argv.begin(), invocation.argv.end(),
                                         "--output-format");
    assert(output_format != invocation.argv.end());
    assert(output_format + 1 != invocation.argv.end());
    assert(*(output_format + 1) == "json");

    const std::string expected_decision =
        R"({"assessment":"Policy inspected.","decision":"idle","next_wake_after_ms":null,"openai_request":null,"reason":"No action required.","schema":"gaudere.cognition.local-goose.decision.v1"})";
    const std::string structured_output = R"({
  "messages": [
    {
      "role": "user",
      "content": [{"type": "text", "text": "inspect policy"}]
    },
    {
      "role": "assistant",
      "content": [
        {"type": "thinking", "thinking": "I will inspect it."},
        {"type": "toolRequest", "toolCall": {"status": "success"}}
      ]
    },
    {
      "role": "user",
      "content": [{"type": "toolResponse", "toolResult": {"status": "success"}}]
    },
    {
      "role": "assistant",
      "content": [
        {"type": "thinking", "thinking": "Inspection complete."},
        {"type": "text", "text": "{\"assessment\":\"Policy inspected.\",\"decision\":\"idle\",\"next_wake_after_ms\":null,\"openai_request\":null,\"reason\":\"No action required.\",\"schema\":\"gaudere.cognition.local-goose.decision.v1\"}"}
      ]
    }
  ],
  "metadata": {"status": "completed"}
})";

    const auto inspected =
        gaudere_agent::inspect_goose_structured_output(structured_output);
    assert(inspected.eligible);
    assert(inspected.response == expected_decision);
    assert(inspected.detail.empty());

    const auto decorated = gaudere_agent::inspect_goose_structured_output(
        std::string{"\n  \xE2\x94\x80\xE2\x94\x80 tool trace\n"} + structured_output);
    assert(!decorated.eligible);

    const auto incomplete = gaudere_agent::inspect_goose_structured_output(
        R"({"messages":[{"role":"assistant","content":[{"type":"text","text":"{}"}]}],"metadata":{"status":"running"}})");
    assert(!incomplete.eligible);

    const auto ambiguous = gaudere_agent::inspect_goose_structured_output(
        R"({"messages":[{"role":"assistant","content":[{"type":"text","text":"one"},{"type":"text","text":"two"}]}],"metadata":{"status":"completed"}})");
    assert(!ambiguous.eligible);

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
