#include "StateLock.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace gaudere_agent {

StateLock::StateLock(const std::string& state_path)
{
    const auto lock_path = state_path + ".lock";
    descriptor_ = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (descriptor_ < 0) {
        throw std::runtime_error("cannot open state ownership lock: "
                                 + std::string(std::strerror(errno)));
    }
    if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const int saved_errno = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            throw std::runtime_error(
                "state database is already owned by another gaudere-agent process");
        }
        throw std::runtime_error("cannot acquire state ownership lock: "
                                 + std::string(std::strerror(saved_errno)));
    }
}

StateLock::~StateLock()
{
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

} // namespace gaudere_agent
