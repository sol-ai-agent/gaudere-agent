#include "GaudereChat.hpp"

#include "LocalGooseDialogueV3.hpp"
#include "Sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/random.h>
#include <thread>
#include <utility>

namespace gaudere_agent {
namespace {

using Json = nlohmann::json;

bool lowercase_hex(const std::string& value, const std::size_t exact = 0) noexcept
{
    if ((exact != 0 && value.size() != exact) || value.empty()) return false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool dialogue_task_id(const std::string& value) noexcept
{
    constexpr const char* v2 = "cognition.local-goose-dialogue.v2:";
    constexpr const char* v3 = "cognition.local-goose-dialogue.v3:";
    for (const char* prefix : {v2, v3}) {
        const std::string p{prefix};
        if (value.size() == p.size() + 64
            && value.compare(0, p.size(), p) == 0
            && lowercase_hex(value.substr(p.size()), 64)) {
            return true;
        }
    }
    return false;
}

bool safe_alias(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 128) return false;
    for (const unsigned char c : value) {
        const bool allowed =
            (c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == ':' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

int hex_digit(const unsigned char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

std::optional<std::string> decode_quoted(const std::string& value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return std::nullopt;
    }
    std::string out;
    out.reserve(value.size() - 2);
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        const unsigned char c = value[i];
        if (c != '\\') {
            out.push_back(static_cast<char>(c));
            continue;
        }
        if (++i + 1 > value.size()) return std::nullopt;
        const unsigned char escaped = value[i];
        switch (escaped) {
        case '\\': out.push_back('\\'); break;
        case '"': out.push_back('"'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'x': {
            if (i + 2 >= value.size() - 1) return std::nullopt;
            const int high = hex_digit(static_cast<unsigned char>(value[i + 1]));
            const int low = hex_digit(static_cast<unsigned char>(value[i + 2]));
            if (high < 0 || low < 0) return std::nullopt;
            out.push_back(static_cast<char>((high << 4) | low));
            i += 2;
            break;
        }
        default:
            return std::nullopt;
        }
    }
    return out;
}

std::map<std::string, std::string> parse_report(const std::string& body)
{
    std::map<std::string, std::string> out;
    std::istringstream input(body);
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos || separator == 0) continue;
        const auto key = line.substr(0, separator);
        auto value = line.substr(separator + 1);
        if (!value.empty() && value.front() == '"') {
            const auto decoded = decode_quoted(value);
            if (!decoded) {
                throw std::invalid_argument("quoted live-control report field is invalid");
            }
            value = *decoded;
        }
        // This parser feeds both the preferred commit proof and the final
        // V3 response verifier. Silent last-value-wins on duplicate keys
        // could turn two contradictory identities/statuses into one trusted
        // report. Reject ALL duplicate keys before interpreting a report.
        if (!out.emplace(key, std::move(value)).second) {
            throw std::invalid_argument("duplicate live-control report field");
        }
    }
    return out;
}

std::optional<std::uint64_t> parse_uint64(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 20) return std::nullopt;
    std::uint64_t out = 0;
    for (const unsigned char c : value) {
        if (c < '0' || c > '9') return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        out = out * 10 + digit;
    }
    return out;
}

std::optional<GaudereChatHead> parse_head(
    const std::string& body,
    const std::string& expected_alias)
{
    try {
        const auto fields = parse_report(body);
        const auto alias = fields.find("alias");
        const auto revision = fields.find("revision");
        const auto root = fields.find("root_task_id");
        const auto head = fields.find("head_task_id");
        if (alias == fields.end() || revision == fields.end()
            || root == fields.end() || head == fields.end()) {
            return std::nullopt;
        }
        const auto parsed_revision = parse_uint64(revision->second);
        if (!parsed_revision || alias->second != expected_alias
            || !dialogue_task_id(root->second)
            || !dialogue_task_id(head->second)) {
            return std::nullopt;
        }
        return GaudereChatHead{
            alias->second, *parsed_revision, root->second, head->second};
    } catch (...) {
        return std::nullopt;
    }
}

bool terminal_status(const std::string& value) noexcept
{
    return value == "succeeded"
        || value == "failed"
        || value == "cancelled"
        || value == "manual_review";
}

bool active_status(const std::string& value) noexcept
{
    return value == "pending"
        || value == "running"
        || value == "cancel_requested";
}

std::optional<std::string> canonical_response(
    const std::string& raw,
    const std::string& request_id,
    const GaudereChatHead& predecessor_head)
{
    try {
        const auto parsed = Json::parse(raw);
        const std::set<std::string> expected{
            "message_kind",
            "model_sha256",
            "predecessor_result_sha256",
            "predecessor_task_id",
            "request_id",
            "response",
            "root_task_id",
            "schema",
            "speaker_id",
            "speaker_kind",
            "turn_index"
        };
        if (!parsed.is_object()) return std::nullopt;
        std::set<std::string> keys;
        for (const auto& item : parsed.items()) keys.insert(item.key());
        if (keys != expected || parsed.dump() != raw
            || parsed.value("schema", "") != local_goose_dialogue_v3_response_schema
            || parsed.value("request_id", "") != request_id
            || parsed.value("speaker_kind", "") != local_goose_dialogue_v3_speaker_human
            || parsed.value("speaker_id", "") != "bertrand"
            || parsed.value("message_kind", "") != local_goose_dialogue_v3_message_dialogue
            || !parsed.at("response").is_string()
            || !parsed.at("turn_index").is_number_unsigned()
            || !parsed.at("root_task_id").is_string()
            || !parsed.at("predecessor_task_id").is_string()
            || !parsed.at("predecessor_result_sha256").is_string()
            || !parsed.at("model_sha256").is_string()) {
            return std::nullopt;
        }
        const auto response = parsed.at("response").get<std::string>();
        const auto root = parsed.at("root_task_id").get<std::string>();
        const auto predecessor =
            parsed.at("predecessor_task_id").get<std::string>();
        const auto predecessor_sha =
            parsed.at("predecessor_result_sha256").get<std::string>();
        const auto model_sha = parsed.at("model_sha256").get<std::string>();
        if (response.empty() || response.size() > 16 * 1024
            || root != predecessor_head.root_task_id
            || predecessor != predecessor_head.head_task_id
            || !lowercase_hex(predecessor_sha, 64)
            || !lowercase_hex(model_sha, 64)) {
            return std::nullopt;
        }
        return response;
    } catch (...) {
        return std::nullopt;
    }
}

GaudereChatTurnResult malformed(
    const std::string& request_id,
    const std::uint64_t expected_revision,
    std::string detail)
{
    GaudereChatTurnResult out;
    out.code = GaudereChatTurnCode::invalid_reply;
    out.request_id = request_id;
    out.expected_revision = expected_revision;
    out.detail = std::move(detail);
    return out;
}

} // namespace

GaudereChatSession::GaudereChatSession(
    GaudereChatTransport transport,
    std::string session_nonce,
    GaudereChatOptions options,
    GaudereChatClock clock,
    GaudereChatSleeper sleeper)
    : transport_(std::move(transport)),
      session_nonce_(std::move(session_nonce)),
      options_(std::move(options)),
      clock_(std::move(clock)),
      sleeper_(std::move(sleeper))
{
    if (!transport_) throw std::invalid_argument("gaudere-chat transport is required");
    if (!lowercase_hex(session_nonce_)
        || session_nonce_.size() < 16 || session_nonce_.size() > 64) {
        throw std::invalid_argument("gaudere-chat session nonce is invalid");
    }
    if (!safe_alias(options_.thread_alias)) {
        throw std::invalid_argument("gaudere-chat thread alias is invalid");
    }
    if (options_.poll_interval.count() <= 0
        || options_.timeout.count() <= 0
        || options_.io_timeout.count() <= 0) {
        throw std::invalid_argument("gaudere-chat timing bounds are invalid");
    }
    if (!clock_) {
        clock_ = [] { return std::chrono::steady_clock::now(); };
    }
    if (!sleeper_) {
        sleeper_ = [](const std::chrono::milliseconds duration) {
            std::this_thread::sleep_for(duration);
        };
    }
}

GaudereChatHeadResult GaudereChatSession::inspect_head() const
{
    LiveControlCommand command;
    command.operation = LiveControlOperation::inspect_local_goose_dialogue_thread_head;
    command.id = options_.thread_alias;
    const auto reply = transport_(command, options_.io_timeout);
    if (reply.code != 0) {
        return GaudereChatHeadResult{
            false, {}, reply.body.empty()
                ? "preferred dialogue head inspection failed"
                : reply.body};
    }
    const auto head = parse_head(reply.body, options_.thread_alias);
    if (!head) {
        return GaudereChatHeadResult{
            false, {}, "preferred dialogue head report is invalid"};
    }
    return GaudereChatHeadResult{true, *head, {}};
}

GaudereChatTaskInspection GaudereChatSession::inspect_task(
    const std::string& task_id) const
{
    GaudereChatTaskInspection out;
    out.task_id = task_id;
    // Only a canonical durable dialogue V3 Task ID is accepted. No
    // arbitrary local Task IDs or user-selected operations are forwarded.
    constexpr const char* v3_prefix = "cognition.local-goose-dialogue.v3:";
    if (task_id.rfind(v3_prefix, 0) != 0 || !dialogue_task_id(task_id)) {
        out.detail = "expected a canonical local-goose-dialogue V3 Task ID";
        return out;
    }

    LiveControlCommand command;
    command.operation = LiveControlOperation::inspect_task;
    command.id = task_id;
    // Unlike the post-commit polling loop, this is exactly one read-only
    // request and uses the independent --io-timeout-ms bound.
    const auto reply = transport_(command, options_.io_timeout);
    if (reply.code != 0) {
        out.detail = reply.body.empty()
            ? "read-only Task inspection did not complete"
            : reply.body;
        return out;
    }

    try {
        // The shared parser rejects any duplicate key, including conflicting
        // identity/status lines. A status-only read never grants ACK authority.
        const auto fields = parse_report(reply.body);
        const auto id = fields.find("id");
        const auto kind = fields.find("kind");
        const auto status = fields.find("status");
        if (id == fields.end() || kind == fields.end() || status == fields.end()
            || id->second != task_id
            || kind->second != local_goose_dialogue_v3_task_kind
            || (!active_status(status->second) && !terminal_status(status->second))) {
            out.detail = "read-only Task identity, kind or status is invalid";
            return out;
        }
        out.ok = true;
        out.status = status->second;
        // Deliberately do not decode or display result_output. The report's
        // status is not proof of a canonical V3 reply, an ACK, or authority.
        return out;
    } catch (const std::exception&) {
        out.detail = "read-only Task report is malformed";
        return out;
    }
}

std::string GaudereChatSession::next_request_id(const std::string& message)
{
    if (next_turn_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("gaudere-chat turn counter exhausted");
    }
    const auto digest = sha256_hex(message).substr(0, 16);
    return "chat-v1-" + session_nonce_ + "-"
        + std::to_string(next_turn_++) + "-" + digest;
}

GaudereChatTurnResult GaudereChatSession::send_turn(const std::string& message)
{
    if (ambiguous_attempt_) {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::transport_ambiguous;
        out.request_id = ambiguous_attempt_->request_id;
        out.expected_revision = ambiguous_attempt_->head.revision;
        out.detail =
            "an earlier submission has ambiguous transport state; retry it exactly first";
        return out;
    }
    if (message.empty() || message.size() > 4096) {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::invalid_reply;
        out.detail = "message must be 1..4096 bytes";
        return out;
    }

    const auto head = inspect_head();
    if (!head.ok) {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::invalid_reply;
        out.detail = head.detail;
        return out;
    }

    Attempt attempt{next_request_id(message), message, head.head};
    return submit_attempt(attempt);
}

GaudereChatTurnResult GaudereChatSession::retry_ambiguous()
{
    if (!ambiguous_attempt_) {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::invalid_reply;
        out.detail = "there is no ambiguous submission to retry";
        return out;
    }
    const auto attempt = *ambiguous_attempt_;
    return submit_attempt(attempt);
}

GaudereChatTurnResult GaudereChatSession::wait_last_committed()
{
    if (!last_committed_) {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::invalid_reply;
        out.detail = "no verified committed V3 Task in this chat session; "
            "use /task ID for status-only lookup after reopening";
        return out;
    }

    // The original request_id, predecessor head and task_id come only from
    // the actual verified commit of this session. A status-only lookup
    // cannot populate this proof context. Never re-submit or ACK here.
    const auto recorded = *last_committed_;
    return wait_for_task(recorded.attempt, recorded.task_id);
}

GaudereChatTurnResult GaudereChatSession::submit_attempt(const Attempt& attempt)
{
    LiveControlCommand command;
    command.operation =
        LiveControlOperation::submit_local_goose_dialogue_v3_preferred_next;
    command.id = attempt.request_id;
    command.thread_alias = attempt.head.alias;
    command.expected_thread_revision = attempt.head.revision;
    command.speaker_kind = local_goose_dialogue_v3_speaker_human;
    command.speaker_id = "bertrand";
    command.message_kind = local_goose_dialogue_v3_message_dialogue;
    command.text = attempt.message;

    const auto reply = transport_(command, options_.io_timeout);
    if (reply.code == 1) {
        ambiguous_attempt_ = attempt;
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::transport_ambiguous;
        out.request_id = attempt.request_id;
        out.expected_revision = attempt.head.revision;
        out.detail = reply.body.empty()
            ? "submission transport state is ambiguous"
            : reply.body;
        return out;
    }

    ambiguous_attempt_.reset();

    if (reply.code != 0) {
        if (reply.code != 4) {
            return malformed(
                attempt.request_id,
                attempt.head.revision,
                reply.body.empty()
                    ? "preferred dialogue submission failed"
                    : reply.body);
        }
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::conflict;
        out.request_id = attempt.request_id;
        out.expected_revision = attempt.head.revision;
        try {
            const auto fields = parse_report(reply.body);
            const auto task = fields.find("id");
            if (task != fields.end() && dialogue_task_id(task->second)) {
                out.task_id = task->second;
                out.orphan_task_may_exist = true;
            }
        } catch (...) {
        }
        const auto refreshed = inspect_head();
        if (refreshed.ok) out.current_revision = refreshed.head.revision;
        out.detail = reply.body.empty()
            ? "preferred dialogue submission was rejected"
            : reply.body;
        return out;
    }

    std::map<std::string, std::string> fields;
    try {
        fields = parse_report(reply.body);
    } catch (...) {
        return malformed(attempt.request_id, attempt.head.revision, "preferred dialogue submission report is invalid");
    }

    const auto id = fields.find("id");
    const auto kind = fields.find("kind");
    const auto alias = fields.find("alias");
    const auto revision = fields.find("revision");
    const auto root = fields.find("root_task_id");
    const auto head_task = fields.find("head_task_id");
    if (id == fields.end() || kind == fields.end()
        || alias == fields.end() || revision == fields.end()
        || root == fields.end() || head_task == fields.end()
        || !dialogue_task_id(id->second)
        || kind->second != local_goose_dialogue_v3_task_kind
        || alias->second != attempt.head.alias
        || root->second != attempt.head.root_task_id
        || head_task->second != id->second) {
        return malformed(attempt.request_id, attempt.head.revision, "preferred dialogue commit report differs");
    }
    const auto committed_revision = parse_uint64(revision->second);
    if (!committed_revision
        || attempt.head.revision == std::numeric_limits<std::uint64_t>::max()
        || *committed_revision != attempt.head.revision + 1) {
        return malformed(attempt.request_id, attempt.head.revision, "preferred dialogue committed revision differs");
    }

    // Record only AFTER parsing and verifying the durable commit response.
    // A timeout of subsequent observation must never erase this identity.
    last_committed_ = CommittedAttempt{attempt, id->second};
    return wait_for_task(attempt, id->second);
}

GaudereChatTurnResult GaudereChatSession::wait_for_task(
    const Attempt& attempt,
    const std::string& task_id)
{
    const auto deadline = clock_() + options_.timeout;
    // A committed Task must never be resubmitted after observation times out.
    // The Task ID was proven by the preferred-thread commit report before
    // entering this read-only loop. Even a malformed later response must not
    // erase that durable identity or imply it is safe to re-submit.
    const auto invalid_committed = [&](std::string detail) {
        auto out = malformed(attempt.request_id, attempt.head.revision,
                             std::move(detail));
        out.task_id = task_id;
        out.current_revision = attempt.head.revision + 1;
        return out;
    };
    const auto timed_out = [&]() {
        GaudereChatTurnResult out;
        out.code = GaudereChatTurnCode::timeout;
        out.request_id = attempt.request_id;
        out.task_id = task_id;
        out.expected_revision = attempt.head.revision;
        out.current_revision = attempt.head.revision + 1;
        out.detail = "submitted Task is still non-terminal after bounded wait";
        return out;
    };
    for (;;) {
        const auto now = clock_();
        if (now >= deadline) return timed_out();
        // Round down to avoid exceeding the remaining monotonic budget.
        // If less than 1 ms remains, fail closed instead of issuing a fresh RPC.
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        if (remaining.count() <= 0) return timed_out();

        LiveControlCommand command;
        command.operation = LiveControlOperation::inspect_task;
        command.id = task_id;
        const auto reply = transport_(command, remaining);
        // A test transport can violate its budget; refuse an overdue
        // success just as the production transport would time out.
        if (clock_() >= deadline) return timed_out();
        if (reply.code != 0) {
            GaudereChatTurnResult out;
            out.code = reply.code == 1
                ? GaudereChatTurnCode::timeout
                : GaudereChatTurnCode::invalid_reply;
            out.request_id = attempt.request_id;
            out.task_id = task_id;
            out.expected_revision = attempt.head.revision;
            out.current_revision = attempt.head.revision + 1;
            out.detail = reply.body.empty()
                ? "submitted Task inspection did not complete within its I/O bound"
                : reply.body;
            return out;
        }

        std::map<std::string, std::string> fields;
        try {
            fields = parse_report(reply.body);
        } catch (...) {
            return invalid_committed("submitted Task report is invalid");
        }
        const auto id = fields.find("id");
        const auto kind = fields.find("kind");
        const auto status = fields.find("status");
        if (id == fields.end() || kind == fields.end() || status == fields.end()
            || id->second != task_id
            || kind->second != local_goose_dialogue_v3_task_kind) {
            return invalid_committed("submitted Task identity differs");
        }

        if (status->second == "succeeded") {
            const auto type = fields.find("result_content_type");
            const auto output = fields.find("result_output");
            if (type == fields.end() || output == fields.end()
                || type->second != local_goose_dialogue_v3_response_content_type) {
                return invalid_committed("successful Task result envelope differs");
            }
            const auto response = canonical_response(
                output->second, attempt.request_id, attempt.head);
            if (!response) {
                return invalid_committed("successful Task response is not canonical v3");
            }
            GaudereChatTurnResult out;
            out.code = GaudereChatTurnCode::succeeded;
            out.request_id = attempt.request_id;
            out.task_id = task_id;
            out.response = *response;
            out.expected_revision = attempt.head.revision;
            out.current_revision = attempt.head.revision + 1;
            return out;
        }

        if (terminal_status(status->second)) {
            GaudereChatTurnResult out;
            out.code = GaudereChatTurnCode::terminal_failure;
            out.request_id = attempt.request_id;
            out.task_id = task_id;
            out.expected_revision = attempt.head.revision;
            out.detail = reply.body;
            return out;
        }
        if (!active_status(status->second)) {
            return invalid_committed("submitted Task status is unknown");
        }

        const auto after = clock_();
        if (after >= deadline) return timed_out();
        const auto sleep_budget = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - after);
        if (sleep_budget.count() <= 0) return timed_out();
        sleeper_(std::min(options_.poll_interval, sleep_budget));
    }
}

std::optional<std::string> GaudereChatSession::last_verified_committed_task_id() const
{
    if (!last_committed_) return std::nullopt;
    return last_committed_->task_id;
}

bool GaudereChatSession::has_ambiguous_attempt() const noexcept
{
    return ambiguous_attempt_.has_value();
}

std::string GaudereChatSession::ambiguous_message() const
{
    return ambiguous_attempt_ ? ambiguous_attempt_->message : std::string{};
}

std::string random_gaudere_chat_session_nonce()
{
    std::array<unsigned char, 16> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::getrandom(
            bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("gaudere-chat cannot obtain OS randomness");
        }
        if (count == 0) {
            throw std::runtime_error("gaudere-chat OS randomness returned EOF");
        }
        offset += static_cast<std::size_t>(count);
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

} // namespace gaudere_agent

std::chrono::milliseconds gaudere_agent::GaudereChatSession::io_timeout() const noexcept
{
    return options_.io_timeout;
}
