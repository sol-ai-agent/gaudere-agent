#include "TaskReport.hpp"

#include <gaudere/work/Task.hpp>

#include <iostream>
#include <sstream>
#include <string>

int main()
{
    gaudere::work::Task task;
    task.id = "line\nbreak";
    task.kind = "local.echo";
    task.status = gaudere::work::TaskStatus::failed;
    task.attempts_started = 1;
    task.limits.max_attempts = 2;
    task.cancel_reason = "stop\tplease";
    task.result = gaudere::work::TaskResult{
        "text/plain", "quoted \"result\"", "failed", "bad\nnews",
        "application/vnd.gaudere.provider-usage+json",
        R"({"input_tokens":3,"output_tokens":2})"};

    std::ostringstream output;
    gaudere_agent::print_task_report(output, task);
    const auto report = output.str();

    const bool ok = report.find("id=\"line\\nbreak\"") != std::string::npos
        && report.find("status=failed") != std::string::npos
        && report.find("attempts=1/2") != std::string::npos
        && report.find("cancel_reason=\"stop\\tplease\"") != std::string::npos
        && report.find("result_output=\"quoted \\\"result\\\"\"") != std::string::npos
        && report.find("failure_message=\"bad\\nnews\"") != std::string::npos
        && report.find("result_metadata_content_type=\"application/vnd.gaudere.provider-usage+json\"")
               != std::string::npos
        && report.find("result_metadata=\"{\\\"input_tokens\\\":3,\\\"output_tokens\\\":2}\"")
               != std::string::npos;

    if (!ok) {
        std::cerr << "Unexpected task report:\n" << report;
        return 1;
    }
    std::cout << "All task report tests passed\n";
    return 0;
}
