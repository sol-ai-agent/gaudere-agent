#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_DISPATCHER_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_DISPATCHER_HPP

#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialoguePreferredSubmitter.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/work/Runtime.hpp>
#include <gaudere/work/Task.hpp>
#include <gaudere/work/TaskStore.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace gaudere_agent {

enum class LocalGooseDialogueResponderDispatchCode {
    accepted,
    duplicate,
    conflict,
    invalid,
    unavailable
};

struct LocalGooseDialogueResponderDispatchResult {
    LocalGooseDialogueResponderDispatchCode result =
        LocalGooseDialogueResponderDispatchCode::invalid;
    std::optional<LocalGooseDialogueResponderIntent> intent;
    std::optional<gaudere::work::Task> task;
    std::optional<LocalGooseDialogueThreadHead> head;
    std::uint64_t consumer_last_sequence = 0;
    bool work_may_be_pending = false;
    std::string detail;
};

using LocalGooseDialogueResponderClock = std::function<std::int64_t()>;

/**
 * Explicit bounded responder dispatcher.
 *
 * This class performs no polling and owns no thread. It acts only when
 * prepare() or dispatch() is called by an already-authorized typed surface.
 * It never invents message content and never auto-renews a lease.
 */
class LocalGooseDialogueResponderDispatcher {
public:
    LocalGooseDialogueResponderDispatcher(
        gaudere::work::Runtime& runtime,
        gaudere::work::TaskStore& task_store,
        LocalGooseDialogueThreadStore& thread_store,
        LocalGooseDialogueCompletionFeedStore& completion_store,
        LocalGooseDialogueResponderStore& responder_store,
        std::string model_sha256,
        std::string consumer_id,
        LocalGooseDialogueResponderClock clock);

    [[nodiscard]] const std::string& consumer_id() const noexcept;

    [[nodiscard]] LocalGooseDialogueResponderDispatchResult prepare(
        const std::string& lease_id,
        std::uint64_t completion_sequence,
        const std::string& message_kind,
        const std::string& message,
        std::int64_t intent_ttl_ms);

    [[nodiscard]] LocalGooseDialogueResponderDispatchResult dispatch(
        const std::string& intent_id);

private:
    gaudere::work::Runtime& runtime_;
    gaudere::work::TaskStore& task_store_;
    LocalGooseDialogueThreadStore& thread_store_;
    LocalGooseDialogueCompletionFeedStore& completion_store_;
    LocalGooseDialogueResponderStore& responder_store_;
    std::string model_sha256_;
    std::string consumer_id_;
    LocalGooseDialogueResponderClock clock_;
};

} // namespace gaudere_agent

#endif
