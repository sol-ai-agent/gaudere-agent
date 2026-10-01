#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderDispatcher.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string command;
    std::string state_path;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string responder_sidecar;
    std::string model_sha256;
    std::string consumer_id;
    std::int64_t now_ms = -1;

    std::string lease_id;
    std::string thread_alias;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string purpose;
    std::string message;
    std::string intent_id;
    std::string request_id;

    std::uint64_t max_turns = 0;
    std::int64_t lease_ttl_ms = 0;
    std::int64_t min_interval_ms = 0;
    std::uint64_t completion_sequence = 0;
    std::int64_t intent_ttl_ms = 0;
    std::uint64_t expected_revision = 0;
};

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " COMMAND --state PATH --thread-sidecar PATH"
        << " --completion-sidecar PATH --responder-sidecar PATH"
        << " --model-sha256 SHA256 --consumer-id ID --now-ms MS [command options]\n"
        << "Commands: lease-create, prepare, dispatch, preferred-from-intent, compete\n";
}

bool safe_identifier(const std::string& value)
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_'
            || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if (!std::isdigit(c) && !(c >= 'a' && c <= 'f')) return false;
    }
    return true;
}

std::int64_t parse_i64(const std::string& value, const char* label)
{
    std::size_t consumed = 0;
    const auto parsed = std::stoll(value, &consumed, 10);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string(label) + " is invalid");
    }
    return parsed;
}

std::uint64_t parse_u64(const std::string& value, const char* label)
{
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument(std::string(label) + " is invalid");
    }
    std::size_t consumed = 0;
    const auto parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()
        || parsed > static_cast<unsigned long long>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument(std::string(label) + " is invalid");
    }
    return static_cast<std::uint64_t>(parsed);
}

Options parse_options(int argc, char* argv[])
{
    if (argc < 2) throw std::invalid_argument("missing proof command");
    Options out;
    out.command = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* label) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(
                    std::string("missing value for ") + label);
            }
            return argv[++i];
        };

        if (arg == "--state") out.state_path = need("--state");
        else if (arg == "--thread-sidecar") {
            out.thread_sidecar = need("--thread-sidecar");
        } else if (arg == "--completion-sidecar") {
            out.completion_sidecar = need("--completion-sidecar");
        } else if (arg == "--responder-sidecar") {
            out.responder_sidecar = need("--responder-sidecar");
        } else if (arg == "--model-sha256") {
            out.model_sha256 = need("--model-sha256");
        } else if (arg == "--consumer-id") {
            out.consumer_id = need("--consumer-id");
        } else if (arg == "--now-ms") {
            out.now_ms = parse_i64(need("--now-ms"), "now-ms");
        } else if (arg == "--lease-id") out.lease_id = need("--lease-id");
        else if (arg == "--thread-alias") {
            out.thread_alias = need("--thread-alias");
        } else if (arg == "--speaker-kind") {
            out.speaker_kind = need("--speaker-kind");
        } else if (arg == "--speaker-id") {
            out.speaker_id = need("--speaker-id");
        } else if (arg == "--message-kind") {
            out.message_kind = need("--message-kind");
        } else if (arg == "--purpose") out.purpose = need("--purpose");
        else if (arg == "--message") out.message = need("--message");
        else if (arg == "--intent-id") out.intent_id = need("--intent-id");
        else if (arg == "--request-id") out.request_id = need("--request-id");
        else if (arg == "--max-turns") {
            out.max_turns = parse_u64(need("--max-turns"), "max-turns");
        } else if (arg == "--lease-ttl-ms") {
            out.lease_ttl_ms =
                parse_i64(need("--lease-ttl-ms"), "lease-ttl-ms");
        } else if (arg == "--min-interval-ms") {
            out.min_interval_ms =
                parse_i64(need("--min-interval-ms"), "min-interval-ms");
        } else if (arg == "--completion-sequence") {
            out.completion_sequence = parse_u64(
                need("--completion-sequence"), "completion-sequence");
        } else if (arg == "--intent-ttl-ms") {
            out.intent_ttl_ms =
                parse_i64(need("--intent-ttl-ms"), "intent-ttl-ms");
        } else if (arg == "--expected-revision") {
            out.expected_revision = parse_u64(
                need("--expected-revision"), "expected-revision");
        } else if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument(
                "unknown or incomplete proof argument: " + arg);
        }
    }

    if (out.state_path.empty() || out.thread_sidecar.empty()
        || out.completion_sidecar.empty() || out.responder_sidecar.empty()
        || !canonical_sha256(out.model_sha256)
        || !safe_identifier(out.consumer_id) || out.now_ms < 0) {
        throw std::invalid_argument("common proof options are incomplete");
    }
    for (const auto* path : {
             &out.state_path, &out.thread_sidecar,
             &out.completion_sidecar, &out.responder_sidecar}) {
        if (path->empty() || path->front() != '/') {
            throw std::invalid_argument("proof paths must be absolute");
        }
    }
    return out;
}

void validate_existing_regular(const std::string& path, const char* label)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            std::string(label) + " must be an existing regular non-symlink file");
    }
}

void validate_paths(const Options& options)
{
    validate_existing_regular(options.state_path, "state");
    validate_existing_regular(options.thread_sidecar, "thread sidecar");
    validate_existing_regular(options.completion_sidecar, "completion sidecar");
    if (std::filesystem::exists(options.responder_sidecar)) {
        validate_existing_regular(options.responder_sidecar, "responder sidecar");
    } else {
        const auto parent =
            std::filesystem::path(options.responder_sidecar).parent_path();
        if (parent.empty() || !std::filesystem::is_directory(parent)) {
            throw std::invalid_argument(
                "responder sidecar parent directory is missing");
        }
    }
}

const char* dispatch_name(
    const gaudere_agent::LocalGooseDialogueResponderDispatchCode code)
{
    using Code = gaudere_agent::LocalGooseDialogueResponderDispatchCode;
    switch (code) {
    case Code::accepted: return "accepted";
    case Code::duplicate: return "duplicate";
    case Code::conflict: return "conflict";
    case Code::invalid: return "invalid";
    case Code::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* preferred_name(
    const gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode code)
{
    using Code = gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode;
    switch (code) {
    case Code::accepted: return "accepted";
    case Code::duplicate: return "duplicate";
    case Code::conflict: return "conflict";
    case Code::invalid: return "invalid";
    case Code::unavailable: return "unavailable";
    }
    return "unknown";
}

void print_lease(
    const gaudere_agent::LocalGooseDialogueResponderLease& lease)
{
    std::cout
        << "lease_id=" << lease.lease_id << '\n'
        << "lease_state=" << static_cast<unsigned>(lease.state) << '\n'
        << "turns_committed=" << lease.turns_committed << '\n'
        << "max_system_turns=" << lease.max_system_turns << '\n';
}

void print_intent(
    const gaudere_agent::LocalGooseDialogueResponderIntent& intent)
{
    std::cout
        << "intent_id=" << intent.intent_id << '\n'
        << "request_id=" << intent.request_id << '\n'
        << "intent_state=" << static_cast<unsigned>(intent.state) << '\n'
        << "completion_sequence=" << intent.completion_sequence << '\n';
    if (!intent.submitted_task_id.empty()) {
        std::cout << "submitted_task_id=" << intent.submitted_task_id << '\n';
    }
}

void print_dispatch(
    const gaudere_agent::LocalGooseDialogueResponderDispatchResult& result)
{
    std::cout
        << "result=" << dispatch_name(result.result) << '\n'
        << "consumer_last_sequence=" << result.consumer_last_sequence << '\n'
        << "detail=" << result.detail << '\n';
    if (result.intent) print_intent(*result.intent);
    if (result.task) std::cout << "task_id=" << result.task->id << '\n';
    if (result.head) {
        std::cout
            << "head_revision=" << result.head->revision << '\n'
            << "head_task_id=" << result.head->head_task_id << '\n';
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        validate_paths(options);

        gaudere::persistence::sqlite::TaskStore task_store(options.state_path);
        const auto runtime_now = [ms = options.now_ms] {
            return std::chrono::system_clock::time_point{
                std::chrono::milliseconds{ms}};
        };
        gaudere::work::Runtime runtime(task_store, runtime_now);
        gaudere_agent::LocalGooseDialogueThreadStore thread_store(
            options.thread_sidecar);
        gaudere_agent::LocalGooseDialogueCompletionFeedStore completion_store(
            options.completion_sidecar);
        gaudere_agent::LocalGooseDialogueResponderStore responder_store(
            options.responder_sidecar);
        gaudere_agent::LocalGooseDialogueResponderDispatcher dispatcher(
            runtime,
            task_store,
            thread_store,
            completion_store,
            responder_store,
            options.model_sha256,
            options.consumer_id,
            [ms = options.now_ms] { return ms; });

        if (options.command == "lease-create") {
            if (!safe_identifier(options.lease_id)
                || !safe_identifier(options.thread_alias)
                || !safe_identifier(options.speaker_id)
                || options.message_kind.empty()
                || options.purpose.empty()
                || options.max_turns == 0
                || options.lease_ttl_ms <= 0
                || options.min_interval_ms < 0) {
                throw std::invalid_argument(
                    "lease-create options are incomplete");
            }
            const auto result = dispatcher.create_lease(
                options.lease_id,
                options.thread_alias,
                options.speaker_id,
                options.message_kind,
                options.purpose,
                options.max_turns,
                options.lease_ttl_ms,
                options.min_interval_ms);
            std::cout
                << "result=" << dispatch_name(result.result) << '\n'
                << "detail=" << result.detail << '\n';
            if (result.lease) print_lease(*result.lease);
            return (result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::accepted
                    || result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::duplicate)
                ? 0 : 4;
        }

        if (options.command == "prepare") {
            if (!safe_identifier(options.lease_id)
                || options.completion_sequence == 0
                || options.message_kind.empty()
                || options.message.empty()
                || options.intent_ttl_ms <= 0) {
                throw std::invalid_argument("prepare options are incomplete");
            }
            const auto result = dispatcher.prepare(
                options.lease_id,
                options.completion_sequence,
                options.message_kind,
                options.message,
                options.intent_ttl_ms);
            print_dispatch(result);
            return (result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::accepted
                    || result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::duplicate)
                ? 0 : 4;
        }

        if (options.command == "dispatch") {
            if (options.intent_id.empty()) {
                throw std::invalid_argument("dispatch intent id is missing");
            }
            const auto result = dispatcher.dispatch(options.intent_id);
            print_dispatch(result);
            return (result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::accepted
                    || result.result
                        == gaudere_agent::LocalGooseDialogueResponderDispatchCode::duplicate)
                ? 0 : 4;
        }

        if (options.command == "preferred-from-intent") {
            const auto intent = responder_store.find_intent(options.intent_id);
            if (!intent
                || intent->state
                    != gaudere_agent::LocalGooseDialogueResponderIntentState::prepared) {
                throw std::invalid_argument(
                    "preferred-from-intent requires a prepared intent");
            }
            const auto result =
                gaudere_agent::submit_local_goose_dialogue_v3_preferred(
                    runtime,
                    task_store,
                    thread_store,
                    options.model_sha256,
                    intent->request_id,
                    intent->thread_alias,
                    intent->expected_thread_revision,
                    intent->speaker_kind,
                    intent->speaker_id,
                    intent->message_kind,
                    intent->message);
            std::cout
                << "result=" << preferred_name(result.result) << '\n'
                << "detail=" << result.detail << '\n';
            if (result.task) {
                std::cout << "task_id=" << result.task->id << '\n';
            }
            if (result.head) {
                std::cout
                    << "head_revision=" << result.head->revision << '\n'
                    << "head_task_id=" << result.head->head_task_id << '\n';
            }
            return (result.result
                        == gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode::accepted
                    || result.result
                        == gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode::duplicate)
                ? 0 : 4;
        }

        if (options.command == "compete") {
            if (!safe_identifier(options.request_id)
                || !safe_identifier(options.thread_alias)
                || options.speaker_kind.empty()
                || !safe_identifier(options.speaker_id)
                || options.message_kind.empty()
                || options.message.empty()) {
                throw std::invalid_argument("compete options are incomplete");
            }
            const auto result =
                gaudere_agent::submit_local_goose_dialogue_v3_preferred(
                    runtime,
                    task_store,
                    thread_store,
                    options.model_sha256,
                    options.request_id,
                    options.thread_alias,
                    options.expected_revision,
                    options.speaker_kind,
                    options.speaker_id,
                    options.message_kind,
                    options.message);
            std::cout
                << "result=" << preferred_name(result.result) << '\n'
                << "detail=" << result.detail << '\n';
            if (result.task) {
                std::cout << "task_id=" << result.task->id << '\n';
            }
            if (result.head) {
                std::cout
                    << "head_revision=" << result.head->revision << '\n'
                    << "head_task_id=" << result.head->head_task_id << '\n';
            }
            return (result.result
                        == gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode::accepted
                    || result.result
                        == gaudere_agent::LocalGooseDialoguePreferredSubmitResultCode::duplicate)
                ? 0 : 4;
        }

        throw std::invalid_argument("unsupported proof command");
    } catch (const std::exception& error) {
        std::cerr
            << "gaudere-local-goose-dialogue-responder-proof: "
            << error.what() << '\n';
        return 1;
    }
}
