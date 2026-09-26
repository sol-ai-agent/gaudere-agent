#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_COMPLETION_FEED_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_COMPLETION_FEED_HPP

#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/work/Task.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

struct sqlite3;

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_completion_event_prefix =
    "cognition.local-goose-dialogue.completion.v1:";
inline constexpr int local_goose_dialogue_completion_feed_schema = 1;

struct LocalGooseDialogueCompletionEvent {
    std::uint64_t sequence = 0;
    std::string event_id;
    std::string thread_alias;
    std::uint64_t thread_revision = 0;
    std::string task_id;
    std::string root_task_id;
    std::uint64_t turn_index = 0;
    std::string request_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string result_sha256;
    std::string response;
    std::int64_t observed_completed_at_ms = 0;
};

[[nodiscard]] bool valid_local_goose_dialogue_completion_event(
    const LocalGooseDialogueCompletionEvent& event) noexcept;

[[nodiscard]] LocalGooseDialogueCompletionEvent
make_local_goose_dialogue_completion_event(
    const std::string& thread_alias,
    std::uint64_t thread_revision,
    const gaudere::work::Task& task,
    std::int64_t observed_completed_at_ms);

enum class LocalGooseDialogueCompletionFeedResult {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueCompletionFeedWrite {
    LocalGooseDialogueCompletionFeedResult result =
        LocalGooseDialogueCompletionFeedResult::invalid;
    std::optional<LocalGooseDialogueCompletionEvent> event;
    std::string detail;
};

struct LocalGooseDialogueCompletionCursorWrite {
    LocalGooseDialogueCompletionFeedResult result =
        LocalGooseDialogueCompletionFeedResult::invalid;
    std::uint64_t last_sequence = 0;
    std::string detail;
};

/**
 * Durable preferred-thread dialogue completion feed.
 *
 * Events are append-only and ordered by SQLite sequence. A per-thread
 * materialization cursor prevents skipped preferred-head revisions. Consumer
 * cursors acknowledge only the next global event, so acknowledged events are
 * not delivered twice after restart. Unacknowledged events may be redelivered.
 *
 * The sidecar owns no Task execution, provider, scheduler, network, shell,
 * stimulus, wake, or model authority.
 */
class LocalGooseDialogueCompletionFeedStore {
public:
    explicit LocalGooseDialogueCompletionFeedStore(const std::string& path);
    ~LocalGooseDialogueCompletionFeedStore();

    LocalGooseDialogueCompletionFeedStore(
        const LocalGooseDialogueCompletionFeedStore&) = delete;
    LocalGooseDialogueCompletionFeedStore& operator=(
        const LocalGooseDialogueCompletionFeedStore&) = delete;

    [[nodiscard]] std::optional<std::uint64_t> materialization_next_revision(
        const std::string& thread_alias) const;

    [[nodiscard]] LocalGooseDialogueCompletionFeedWrite append_for_revision(
        const LocalGooseDialogueCompletionEvent& event);

    [[nodiscard]] std::optional<LocalGooseDialogueCompletionEvent>
    next_for_consumer(const std::string& consumer_id) const;

    [[nodiscard]] LocalGooseDialogueCompletionCursorWrite acknowledge(
        const std::string& consumer_id,
        std::uint64_t sequence);

private:
    sqlite3* database_ = nullptr;
};

struct LocalGooseDialogueCompletionReconcileResult {
    std::size_t materialized = 0;
    bool pending = false;
    bool blocked = false;
    std::string detail;
};

using LocalGooseDialogueCompletionTaskLookup =
    std::function<std::optional<gaudere::work::Task>(const std::string&)>;
using LocalGooseDialogueCompletionClock =
    std::function<std::int64_t()>;

/**
 * Reconcile preferred thread revisions to completion events by exact Task ID.
 *
 * This never enumerates the Core Task database. It follows only the immutable
 * preferred-head revision journal and stops at the first Task that is not yet a
 * canonical success.
 */
class LocalGooseDialogueCompletionFeedService {
public:
    LocalGooseDialogueCompletionFeedService(
        LocalGooseDialogueThreadStore& thread_store,
        LocalGooseDialogueCompletionFeedStore& feed_store,
        LocalGooseDialogueCompletionTaskLookup lookup,
        LocalGooseDialogueCompletionClock clock);

    [[nodiscard]] LocalGooseDialogueCompletionReconcileResult reconcile(
        const std::string& thread_alias,
        std::size_t limit = 64);

private:
    LocalGooseDialogueThreadStore& thread_store_;
    LocalGooseDialogueCompletionFeedStore& feed_store_;
    LocalGooseDialogueCompletionTaskLookup lookup_;
    LocalGooseDialogueCompletionClock clock_;
};

} // namespace gaudere_agent

#endif
