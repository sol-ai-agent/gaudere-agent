#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_PREFERRED_SUBMITTER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_PREFERRED_SUBMITTER_HPP

#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialoguePreferredSubmitResultCode {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialoguePreferredSubmitResult {
    LocalGooseDialoguePreferredSubmitResultCode result =
        LocalGooseDialoguePreferredSubmitResultCode::invalid;
    std::optional<gaudere::work::Task> task;
    std::optional<LocalGooseDialogueThreadHead> head;
    bool work_may_be_pending = false;
    std::string detail;
};

/**
 * Canonical preferred-head V3 submission primitive.
 *
 * This is the structured equivalent of local-thread-v3-send. It owns no
 * provider, feed, responder-lease, scheduler, wake, stimulus, network, or shell
 * authority. The preferred-head CAS remains the final serialization point.
 */
[[nodiscard]] LocalGooseDialoguePreferredSubmitResult
submit_local_goose_dialogue_v3_preferred(
    gaudere::work::Runtime& runtime,
    gaudere::work::TaskStore& task_store,
    LocalGooseDialogueThreadStore& thread_store,
    const std::string& model_sha256,
    const std::string& request_id,
    const std::string& thread_alias,
    std::uint64_t expected_thread_revision,
    const std::string& speaker_kind,
    const std::string& speaker_id,
    const std::string& message_kind,
    const std::string& message);

} // namespace gaudere_agent

#endif
