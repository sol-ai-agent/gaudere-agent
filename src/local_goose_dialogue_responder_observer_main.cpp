#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderObserver.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Action = gaudere_agent::LocalGooseDialogueResponderObserverAction;
using Classification = gaudere_agent::LocalGooseDialogueResponderObserverClassification;
using Disposition = gaudere_agent::LocalGooseDialogueResponderDisposition;
using DispositionKind = gaudere_agent::LocalGooseDialogueResponderDispositionKind;
using DispositionState = gaudere_agent::LocalGooseDialogueResponderDispositionState;

constexpr const char* consumer_id = "system-responder-v1";
constexpr const char* thread_alias = "main";
constexpr const char* authority_source = "observer-policy-v1";

struct Options {
    std::string command;
    std::string thread_sidecar;
    std::string completion_sidecar;
    std::string responder_sidecar;
    std::uint64_t expected_sequence = 0;
    std::string expected_event_id;
    std::string expected_result_sha256;
    bool structural_retirement_authorized = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " status --thread-sidecar PATH --completion-sidecar PATH"
        << " --responder-sidecar PATH\n"
        << "       " << program
        << " step --thread-sidecar PATH --completion-sidecar PATH"
        << " --responder-sidecar PATH --expected-sequence N"
        << " --expected-event-id ID --expected-result-sha256 SHA256"
        << " --authorize-structural-retirement\n";
}

bool canonical_sha256(const std::string& value)
{
    if (value.size() != 64) return false;
    for (const unsigned char c : value) {
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return false;
    }
    return true;
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
    if (argc >= 2 && std::string{argv[1]} == "--help") {
        usage(argv[0]);
        std::exit(0);
    }
    if (argc < 2) throw std::invalid_argument("missing observer command");

    Options options;
    options.command = argv[1];
    if (options.command != "status" && options.command != "step") {
        throw std::invalid_argument("observer command must be status or step");
    }

    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        auto need = [&](const char* label) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument(
                    std::string("missing value for ") + label);
            }
            return argv[++index];
        };

        if (argument == "--thread-sidecar") {
            options.thread_sidecar = need("--thread-sidecar");
        } else if (argument == "--completion-sidecar") {
            options.completion_sidecar = need("--completion-sidecar");
        } else if (argument == "--responder-sidecar") {
            options.responder_sidecar = need("--responder-sidecar");
        } else if (argument == "--expected-sequence") {
            options.expected_sequence = parse_u64(
                need("--expected-sequence"), "expected-sequence");
        } else if (argument == "--expected-event-id") {
            options.expected_event_id = need("--expected-event-id");
        } else if (argument == "--expected-result-sha256") {
            options.expected_result_sha256 = need("--expected-result-sha256");
        } else if (argument == "--authorize-structural-retirement") {
            options.structural_retirement_authorized = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown observer argument: " + argument);
        }
    }

    for (const auto* path : {
             &options.thread_sidecar,
             &options.completion_sidecar,
             &options.responder_sidecar}) {
        if (path->empty() || path->front() != '/') {
            throw std::invalid_argument(
                "observer sidecar paths must be absolute");
        }
    }

    if (options.command == "status") {
        if (options.expected_sequence != 0
            || !options.expected_event_id.empty()
            || !options.expected_result_sha256.empty()
            || options.structural_retirement_authorized) {
            throw std::invalid_argument(
                "status is read-only and does not accept step authority");
        }
    } else {
        if (!options.structural_retirement_authorized
            || options.expected_sequence == 0
            || options.expected_event_id.empty()
            || !canonical_sha256(options.expected_result_sha256)) {
            throw std::invalid_argument(
                "step requires explicit structural authority and exact event evidence");
        }
    }

    return options;
}

void require_regular_non_symlink(const std::string& path, const char* label)
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

void print_status(
    const gaudere_agent::LocalGooseDialogueResponderObserverStatus& status)
{
    std::cout
        << "classification=" << classification_name(status.classification) << '\n'
        << "consumer_id=" << status.consumer_id << '\n'
        << "thread_alias=" << status.thread_alias << '\n'
        << "observer_armed=" << (status.observation_armed ? "yes" : "no") << '\n';
    if (status.consumer_last_sequence) {
        std::cout << "consumer_last_sequence="
                  << *status.consumer_last_sequence << '\n';
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
            << "disposition_kind="
            << static_cast<unsigned>(status.disposition->kind) << '\n'
            << "disposition_state="
            << static_cast<unsigned>(status.disposition->state) << '\n';
    }
    std::cout << "status_detail=" << status.detail << '\n';
}

bool exact_expected_event(
    const gaudere_agent::LocalGooseDialogueCompletionEvent& event,
    const Options& options) noexcept
{
    return event.sequence == options.expected_sequence
        && event.event_id == options.expected_event_id
        && event.result_sha256 == options.expected_result_sha256
        && event.thread_alias == thread_alias;
}

bool earliest_prepared_matches_expected(
    gaudere_agent::LocalGooseDialogueResponderStore& store,
    const Options& options)
{
    const auto prepared = store.dispositions_in_state(
        DispositionState::prepared, 1024);
    const Disposition* earliest = nullptr;
    for (const auto& disposition : prepared) {
        if (disposition.consumer_id != consumer_id) continue;
        if (!earliest
            || disposition.completion_sequence < earliest->completion_sequence
            || (disposition.completion_sequence == earliest->completion_sequence
                && disposition.disposition_id < earliest->disposition_id)) {
            earliest = &disposition;
        }
    }
    if (!earliest) return false;
    return earliest->completion_sequence == options.expected_sequence
        && earliest->event_id == options.expected_event_id
        && earliest->result_sha256 == options.expected_result_sha256
        && earliest->thread_alias == thread_alias
        && earliest->kind != DispositionKind::intervention;
}

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        require_regular_non_symlink(options.thread_sidecar, "thread sidecar");
        require_regular_non_symlink(options.completion_sidecar, "completion sidecar");
        require_regular_non_symlink(options.responder_sidecar, "responder sidecar");

        gaudere_agent::LocalGooseDialogueThreadStore thread_store(
            options.thread_sidecar);
        gaudere_agent::LocalGooseDialogueCompletionFeedStore completion_store(
            options.completion_sidecar);
        // Deliberately preserve the existing schema. Production schema upgrade is
        // a separate explicit activation transition and cannot occur here.
        gaudere_agent::LocalGooseDialogueResponderStore responder_store(
            options.responder_sidecar);

        gaudere_agent::LocalGooseDialogueResponderObserverConfig config;
        config.consumer_id = consumer_id;
        config.thread_alias = thread_alias;
        config.structural_authority_source = authority_source;
        config.observation_armed = options.command == "step";
        gaudere_agent::LocalGooseDialogueResponderObserver observer(
            completion_store, thread_store, responder_store, config, now_ms);

        if (options.command == "status") {
            const auto status = observer.inspect();
            std::cout << "action=status\n";
            print_status(status);
            return status.classification == Classification::ambiguous ? 4 : 0;
        }

        const auto cursor = completion_store.consumer_last_sequence(consumer_id)
            .value_or(0);
        if (cursor == options.expected_sequence) {
            if (!earliest_prepared_matches_expected(responder_store, options)) {
                throw std::runtime_error(
                    "cursor reached expected sequence without exact earliest prepared disposition");
            }
        } else {
            if (cursor == std::numeric_limits<std::uint64_t>::max()
                || cursor + 1 != options.expected_sequence) {
                throw std::runtime_error(
                    "consumer cursor is outside exact expected transition");
            }
            const auto next = completion_store.next_for_consumer(consumer_id);
            if (!next || !exact_expected_event(*next, options)) {
                throw std::runtime_error(
                    "next completion does not match exact expected event evidence");
            }
        }

        const auto result = observer.step();
        std::cout << "action=" << action_name(result.action) << '\n'
                  << "detail=" << result.detail << '\n';
        print_status(result.status);

        if (!(result.action == Action::acknowledged
              || result.action == Action::reconciled)) {
            return result.action == Action::manual_review ? 5 : 4;
        }
        const auto after = completion_store.consumer_last_sequence(consumer_id)
            .value_or(0);
        if (after != options.expected_sequence) {
            throw std::runtime_error(
                "observer step did not retire exactly the expected sequence");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-dialogue-responder-observer: "
                  << error.what() << '\n';
        return 1;
    }
}
