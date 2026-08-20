#include "TaskReport.hpp"

#include <chrono>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace gaudere_agent {

const char* task_status_name(const gaudere::work::TaskStatus status) noexcept
{
    using gaudere::work::TaskStatus;
    switch (status) {
    case TaskStatus::pending:
        return "pending";
    case TaskStatus::running:
        return "running";
    case TaskStatus::cancel_requested:
        return "cancel_requested";
    case TaskStatus::succeeded:
        return "succeeded";
    case TaskStatus::failed:
        return "failed";
    case TaskStatus::cancelled:
        return "cancelled";
    case TaskStatus::manual_review:
        return "manual_review";
    }
    return "unknown";
}

std::string escaped_text(const std::string& value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char byte : value) {
        switch (byte) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20) {
                output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned int>(byte) << std::dec;
            } else {
                output << static_cast<char>(byte);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

void print_task_report(std::ostream& output, const gaudere::work::Task& task)
{
    output << "id=" << escaped_text(task.id) << '\n'
           << "kind=" << escaped_text(task.kind) << '\n'
           << "status=" << task_status_name(task.status) << '\n'
           << "attempts=" << task.attempts_started << '/' << task.limits.max_attempts << '\n';

    if (task.lease) {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
            task.lease->expires_at.time_since_epoch()).count();
        output << "lease_owner=" << escaped_text(task.lease->owner) << '\n'
               << "lease_expires_unix_ms=" << milliseconds << '\n';
    }
    if (!task.cancel_reason.empty()) {
        output << "cancel_reason=" << escaped_text(task.cancel_reason) << '\n';
    }
    if (task.result) {
        output << "result_content_type=" << escaped_text(task.result->content_type) << '\n'
               << "result_output=" << escaped_text(task.result->output) << '\n';
        if (!task.result->failure_code.empty()) {
            output << "failure_code=" << escaped_text(task.result->failure_code) << '\n';
        }
        if (!task.result->failure_message.empty()) {
            output << "failure_message=" << escaped_text(task.result->failure_message) << '\n';
        }
        if (!task.result->metadata_content_type.empty()) {
            output << "result_metadata_content_type="
                   << escaped_text(task.result->metadata_content_type) << '\n'
                   << "result_metadata=" << escaped_text(task.result->metadata) << '\n';
        }
    }
}

} // namespace gaudere_agent
