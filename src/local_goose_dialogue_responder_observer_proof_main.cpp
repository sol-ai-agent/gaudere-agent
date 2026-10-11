#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderObserver.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Classification = gaudere_agent::LocalGooseDialogueResponderObserverClassification;
using Action = gaudere_agent::LocalGooseDialogueResponderObserverAction;
using DispositionKind = gaudere_agent::LocalGooseDialogueResponderDispositionKind;
using DispositionState = gaudere_agent::LocalGooseDialogueResponderDispositionState;
using StoreResult = gaudere_agent::LocalGooseDialogueResponderStoreResult;
using FeedResult = gaudere_agent::LocalGooseDialogueCompletionFeedResult;

struct Options {
    std::string command;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string responder_sidecar;
    std::string consumer_id = "system-responder-v1";
    std::string thread_alias = "main";
    std::string authority_source = "observer-proof-v1";
    std::uint64_t expected_sequence = 0;
    std::int64_t now_ms = -1;
};

void usage(const char* program)
{
    std::cerr
        << "Usage: " << program
        << " COMMAND --thread-sidecar PATH --completion-sidecar PATH"
        << " --responder-sidecar PATH --now-ms MS"
        << " [--consumer-id ID] [--thread-alias ALIAS]"
        << " [--authority-source ID] [--expected-sequence N]\n"
        << "Commands: migrate, inspect, step, prepare-system, ack-prepared\n";
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
    if (consumed != value.size()) {
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

        if (arg == "--thread-sidecar") out.thread_sidecar = need("--thread-sidecar");
        else if (arg == "--completion-sidecar") out.completion_sidecar = need("--completion-sidecar");
        else if (arg == "--responder-sidecar") out.responder_sidecar = need("--responder-sidecar");
        else if (arg == "--consumer-id") out.consumer_id = need("--consumer-id");
        else if (arg == "--thread-alias") out.thread_alias = need("--thread-alias");
        else if (arg == "--authority-source") out.authority_source = need("--authority-source");
        else if (arg == "--expected-sequence") {
            out.expected_sequence = parse_u64(need("--expected-sequence"), "expected-sequence");
        } else if (arg == "--now-ms") {
            out.now_ms = parse_i64(need("--now-ms"), "now-ms");
        } else if (arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown proof argument: " + arg);
        }
    }

    if (out.thread_sidecar.empty() || out.completion_sidecar.empty()
        || out.responder_sidecar.empty() || out.now_ms < 0
        || !safe_identifier(out.consumer_id) || !safe_identifier(out.thread_alias)
        || !safe_identifier(out.authority_source)) {
        throw std::invalid_argument("common proof options are incomplete");
    }
    for (const auto* path : {&out.thread_sidecar, &out.completion_sidecar, &out.responder_sidecar}) {
        if (path->empty() || path->front() != '/') {
            throw std::invalid_argument("proof paths must be absolute");
        }
    }
    return out;
}

void validate_regular(const std::string& path, const char* label)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            std::string(label) + " must be an existing regular non-symlink file");
    }
}

const char* classification_name(const Classification value)
{
    switch (value) {
    case Classification::no_event: return "no_event";
    case Classification::structural_system: return "structural_system";
    case Classification::structural_stale: return "structural_stale";
    case Classification::current_human: return "current_human";
    case Classification::intervention_owned: return "intervention_owned";
    case Classification::reconciliation_pending: return "reconciliation_pending";
    case Classification::ambiguous: return "ambiguous";
    }
    return "unknown";
}

const char* action_name(const Action value)
{
    switch (value) {
    case Action::none: return "none";
    case Action::disarmed: return "disarmed";
    case Action::held: return "held";
    case Action::acknowledged: return "acknowledged";
    case Action::reconciled: return "reconciled";
    case Action::manual_review: return "manual_review";
    case Action::unavailable: return "unavailable";
    }
    return "unknown";
}

void print_status(const gaudere_agent::LocalGooseDialogueResponderObserverStatus& status)
{
    std::cout
        << "classification=" << classification_name(status.classification) << '\n'
        << "consumer_id=" << status.consumer_id << '\n'
        << "thread_alias=" << status.thread_alias << '\n'
        << "observer_armed=" << (status.observation_armed ? "yes" : "no") << '\n';
    if (status.consumer_last_sequence) {
        std::cout << "consumer_last_sequence=" << *status.consumer_last_sequence << '\n';
    }
    if (status.next_event) {
        std::cout
            << "next_sequence=" << status.next_event->sequence << '\n'
            << "next_event_id=" << status.next_event->event_id << '\n'
            << "next_result_sha256=" << status.next_event->result_sha256 << '\n'
            << "next_thread_revision=" << status.next_event->thread_revision << '\n'
            << "next_task_id=" << status.next_event->task_id << '\n'
            << "next_speaker_kind=" << status.next_event->speaker_kind << '\n'
            << "next_speaker_id=" << status.next_event->speaker_id << '\n';
    }
    if (status.preferred_head) {
        std::cout
            << "head_revision=" << status.preferred_head->revision << '\n'
            << "head_task_id=" << status.preferred_head->head_task_id << '\n';
    }
    if (status.disposition) {
        std::cout
            << "disposition_id=" << status.disposition->disposition_id << '\n'
            << "disposition_kind=" << static_cast<unsigned>(status.disposition->kind) << '\n'
            << "disposition_state=" << static_cast<unsigned>(status.disposition->state) << '\n';
    }
    std::cout << "status_detail=" << status.detail << '\n';
}

void require_sequence(
    const Options& options,
    const gaudere_agent::LocalGooseDialogueCompletionEvent& event)
{
    if (options.expected_sequence != 0 && event.sequence != options.expected_sequence) {
        throw std::runtime_error("next completion sequence differs from proof expectation");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        validate_regular(options.thread_sidecar, "thread sidecar");
        validate_regular(options.completion_sidecar, "completion sidecar");
        validate_regular(options.responder_sidecar, "responder sidecar");

        if (options.command == "migrate") {
            gaudere_agent::LocalGooseDialogueResponderStore responder_store(
                options.responder_sidecar,
                gaudere_agent::LocalGooseDialogueResponderStoreOpenMode::allow_schema_upgrade);
            std::cout << "result=migrated_or_current\n";
            return 0;
        }

        gaudere_agent::LocalGooseDialogueThreadStore thread_store(options.thread_sidecar);
        gaudere_agent::LocalGooseDialogueCompletionFeedStore completion_store(options.completion_sidecar);
        gaudere_agent::LocalGooseDialogueResponderStore responder_store(options.responder_sidecar);

        gaudere_agent::LocalGooseDialogueResponderObserverConfig config;
        config.consumer_id = options.consumer_id;
        config.thread_alias = options.thread_alias;
        config.structural_authority_source = options.authority_source;
        config.observation_armed = options.command == "step";
        gaudere_agent::LocalGooseDialogueResponderObserver observer(
            completion_store, thread_store, responder_store, config,
            [ms = options.now_ms] { return ms; });

        if (options.command == "inspect") {
            const auto status = observer.inspect();
            if (status.next_event) require_sequence(options, *status.next_event);
            print_status(status);
            return status.classification == Classification::ambiguous ? 4 : 0;
        }

        if (options.command == "step") {
            const auto before = observer.inspect();
            if (before.next_event) require_sequence(options, *before.next_event);
            const auto result = observer.step();
            std::cout << "action=" << action_name(result.action) << '\n'
                      << "detail=" << result.detail << '\n';
            print_status(result.status);
            return (result.action == Action::unavailable || result.action == Action::manual_review)
                ? 4 : 0;
        }

        const auto next = completion_store.next_for_consumer(options.consumer_id);
        if (!next) throw std::runtime_error("proof consumer has no next event");
        require_sequence(options, *next);

        if (options.command == "prepare-system") {
            const auto head = thread_store.find(options.thread_alias);
            if (!head || next->thread_alias != options.thread_alias
                || next->root_task_id != head->root_task_id
                || next->thread_revision > head->revision
                || next->speaker_kind != "system") {
                throw std::runtime_error(
                    "prepare-system requires exact canonical structural system event");
            }
            auto disposition = gaudere_agent::make_local_goose_dialogue_responder_disposition(
                options.consumer_id, next->thread_alias, next->sequence,
                next->event_id, next->result_sha256, next->thread_revision,
                next->task_id, next->speaker_kind, next->speaker_id,
                DispositionKind::structural_skip_system,
                options.authority_source, options.now_ms);
            const auto write = responder_store.prepare_disposition(disposition);
            std::cout << "result=" << static_cast<unsigned>(write.result) << '\n'
                      << "detail=" << write.detail << '\n';
            if (write.disposition) {
                std::cout << "disposition_id=" << write.disposition->disposition_id << '\n'
                          << "disposition_state=" << static_cast<unsigned>(write.disposition->state) << '\n';
            }
            return (write.result == StoreResult::accepted || write.result == StoreResult::duplicate)
                ? 0 : 4;
        }

        if (options.command == "ack-prepared") {
            const auto disposition = responder_store.find_disposition_for_event(
                options.consumer_id, next->event_id);
            if (!disposition
                || disposition->kind != DispositionKind::structural_skip_system
                || disposition->state != DispositionState::prepared
                || disposition->completion_sequence != next->sequence
                || disposition->result_sha256 != next->result_sha256
                || disposition->task_id != next->task_id) {
                throw std::runtime_error(
                    "ack-prepared requires exact prepared structural disposition");
            }
            const auto write = completion_store.acknowledge(options.consumer_id, next->sequence);
            std::cout << "result=" << static_cast<unsigned>(write.result) << '\n'
                      << "last_sequence=" << write.last_sequence << '\n'
                      << "detail=" << write.detail << '\n';
            return (write.result == FeedResult::accepted || write.result == FeedResult::duplicate)
                ? 0 : 4;
        }

        throw std::invalid_argument("unknown proof command");
    } catch (const std::exception& error) {
        std::cerr << "gaudere observer proof: " << error.what() << '\n';
        return 2;
    }
}
