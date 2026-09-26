#ifndef GAUDERE_AGENT_LIVE_CONTROL_HPP
#define GAUDERE_AGENT_LIVE_CONTROL_HPP

#include <fcntl.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gaudere_agent {

enum class LiveControlOperation {
    submit_echo,
    submit_openai,
    submit_reflection,
    submit_local_goose_dialogue,
    submit_local_goose_dialogue_v2_root,
    submit_local_goose_dialogue_v2_next,
    submit_local_goose_dialogue_v3_root,
    submit_local_goose_dialogue_v3_next,
    bind_local_goose_dialogue_thread_head,
    inspect_local_goose_dialogue_thread_head,
    submit_local_goose_dialogue_v3_preferred_next,
    inspect_task,
    inspect_budget,
    accept_wake,
    revoke_wake,
    inspect_wake,
    inspect_wake_status,
    stimulate_local_goose_cycle
};

struct LiveControlCommand {
    LiveControlCommand() = default;

    LiveControlCommand(LiveControlOperation operation_value,
                       std::string id_value,
                       std::string text_value = {},
                       std::string predecessor_task_id_value = {},
                       std::string speaker_kind_value = {},
                       std::string speaker_id_value = {},
                       std::string message_kind_value = {},
                       std::string thread_alias_value = {},
                       std::optional<std::uint64_t> expected_thread_revision_value = std::nullopt)
        : operation(operation_value),
          id(std::move(id_value)),
          text(std::move(text_value)),
          predecessor_task_id(std::move(predecessor_task_id_value)),
          speaker_kind(std::move(speaker_kind_value)),
          speaker_id(std::move(speaker_id_value)),
          message_kind(std::move(message_kind_value)),
          thread_alias(std::move(thread_alias_value)),
          expected_thread_revision(expected_thread_revision_value)
    {
    }

    LiveControlOperation operation = LiveControlOperation::inspect_task;
    std::string id;
    std::string text;
    std::string predecessor_task_id;
    std::string speaker_kind;
    std::string speaker_id;
    std::string message_kind;
    std::string thread_alias;
    std::optional<std::uint64_t> expected_thread_revision;
};

struct LiveControlReply {
    bool ok = false;
    int code = 1;
    std::string body;
};

class PendingLiveControl {
public:
    explicit PendingLiveControl(LiveControlCommand command);

    PendingLiveControl(const PendingLiveControl&) = delete;
    PendingLiveControl& operator=(const PendingLiveControl&) = delete;

    [[nodiscard]] const LiveControlCommand& command() const noexcept;
    void complete(LiveControlReply reply);
    [[nodiscard]] LiveControlReply wait();

private:
    LiveControlCommand command_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool completed_ = false;
    LiveControlReply reply_;
};

/** Thread-safe in-memory handoff from the socket thread to the sole Runtime worker. */
class LiveControlMailbox {
public:
    [[nodiscard]] std::shared_ptr<PendingLiveControl> submit(LiveControlCommand command);
    [[nodiscard]] std::vector<std::shared_ptr<PendingLiveControl>> take_all();
    void stop();

private:
    std::mutex mutex_;
    bool stopped_ = false;
    std::deque<std::shared_ptr<PendingLiveControl>> pending_;
};

/** Local AF_UNIX listener. It never touches Runtime or SQLite. */
class LiveControlServer {
public:
    LiveControlServer(std::string socket_path,
                      LiveControlMailbox& mailbox,
                      std::function<void()> wake_worker);
    ~LiveControlServer();

    LiveControlServer(const LiveControlServer&) = delete;
    LiveControlServer& operator=(const LiveControlServer&) = delete;

    [[nodiscard]] bool start();
    void stop();
    void join();

private:
    void run();

    std::string socket_path_;
    LiveControlMailbox& mailbox_;
    std::function<void()> wake_worker_;
    int listen_fd_ = -1;
    int stop_pipe_read_ = -1;
    int stop_pipe_write_ = -1;
    std::thread thread_;
    bool started_ = false;
};

/** One-shot client used by gaudere-control. */
int run_live_control_client(const std::string& socket_path,
                            const LiveControlCommand& command,
                            std::ostream& output,
                            std::ostream& error);

} // namespace gaudere_agent

#endif
