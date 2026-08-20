#ifndef GAUDERE_AGENT_LIVE_CONTROL_HPP
#define GAUDERE_AGENT_LIVE_CONTROL_HPP

#include <fcntl.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gaudere_agent {

enum class LiveControlOperation {
    submit_echo,
    submit_openai,
    inspect_task,
    inspect_budget
};

struct LiveControlCommand {
    LiveControlOperation operation = LiveControlOperation::inspect_task;
    std::string id;
    std::string text;
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
