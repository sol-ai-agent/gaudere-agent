#ifndef GAUDERE_AGENT_STATE_LOCK_HPP
#define GAUDERE_AGENT_STATE_LOCK_HPP

#include <string>

namespace gaudere_agent {

class StateLock {
public:
    explicit StateLock(const std::string& state_path);
    ~StateLock();

    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
    StateLock(StateLock&&) = delete;
    StateLock& operator=(StateLock&&) = delete;

private:
    int descriptor_ = -1;
};

} // namespace gaudere_agent

#endif
