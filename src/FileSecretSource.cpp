#include "FileSecretSource.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace gaudere_agent {
namespace {

class FileDescriptor final {
public:
    explicit FileDescriptor(const int descriptor) noexcept
        : descriptor_(descriptor)
    {
    }

    ~FileDescriptor()
    {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

[[noreturn]] void io_error(const std::string& context, const int error)
{
    throw std::runtime_error(context + ": " + std::strerror(error));
}

} // namespace

FileSecretSource::FileSecretSource(std::string directory,
                                   const std::size_t max_secret_bytes)
    : directory_(std::move(directory)),
      max_secret_bytes_(max_secret_bytes)
{
    if (directory_.empty()) {
        throw std::invalid_argument("secret directory must not be empty");
    }
    if (max_secret_bytes_ == 0) {
        throw std::invalid_argument("maximum secret size must be greater than zero");
    }

    directory_fd_ = ::open(directory_.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd_ < 0) {
        io_error("cannot open secret directory", errno);
    }
}

FileSecretSource::~FileSecretSource()
{
    if (directory_fd_ >= 0) {
        ::close(directory_fd_);
    }
}

bool FileSecretSource::valid_name(const std::string_view name) noexcept
{
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const unsigned char character : name) {
        if (!std::isalnum(character) && character != '_' && character != '-'
            && character != '.') {
            return false;
        }
    }
    return true;
}

std::optional<SecretValue> FileSecretSource::load(const std::string_view name)
{
    if (!valid_name(name)) {
        throw std::invalid_argument("invalid secret name");
    }

    const std::string file_name{name};
    const int descriptor = ::openat(directory_fd_, file_name.c_str(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        const int saved_error = errno;
        if (saved_error == ENOENT) {
            return std::nullopt;
        }
        io_error("cannot open requested secret", saved_error);
    }
    FileDescriptor file{descriptor};

    struct stat status {};
    if (::fstat(file.get(), &status) != 0) {
        io_error("cannot inspect requested secret", errno);
    }
    if (!S_ISREG(status.st_mode)) {
        throw std::runtime_error("requested secret is not a regular file");
    }
    if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        throw std::runtime_error(
            "requested secret grants permissions to group or other users");
    }
    if (status.st_size < 0
        || static_cast<unsigned long long>(status.st_size) > max_secret_bytes_) {
        throw std::runtime_error("requested secret exceeds the configured size limit");
    }

    std::vector<char> bytes(static_cast<std::size_t>(status.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + offset,
                                  bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            SecretValue scrub{std::move(bytes)};
            io_error("cannot read requested secret", errno);
        }
        if (count == 0) {
            bytes.resize(offset);
            break;
        }
        offset += static_cast<std::size_t>(count);
    }

    char extra = 0;
    for (;;) {
        const auto count = ::read(file.get(), &extra, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            SecretValue scrub{std::move(bytes)};
            io_error("cannot finish reading requested secret", errno);
        }
        if (count > 0) {
            extra = 0;
            SecretValue scrub{std::move(bytes)};
            throw std::runtime_error(
                "requested secret changed while being read or exceeds the configured size limit");
        }
        break;
    }

    return SecretValue{std::move(bytes)};
}

} // namespace gaudere_agent
