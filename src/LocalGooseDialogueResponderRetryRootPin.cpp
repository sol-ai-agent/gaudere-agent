#include "LocalGooseDialogueResponderRetryRootPin.hpp"

#include "Sha256.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept { const int old = fd_; fd_ = -1; return old; }
    void reset(const int fd) noexcept {
        if (fd_ >= 0) close(fd_);
        fd_ = fd;
    }
private:
    int fd_;
};

bool lower_hex(const std::string& value, const std::size_t length) noexcept
{
    if (value.size() != length) return false;
    for (const unsigned char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

unsigned char decode(const char c) noexcept
{
    return static_cast<unsigned char>(c <= '9' ? c - '0' : c - 'a' + 10);
}

bool canonical_path(const std::string& path)
{
    if (path.size() < 3 || path.size() > 4096 || path.front() != '/'
        || path.find('\0') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto stop = path.find('/', start);
        const auto n = (stop == std::string::npos ? path.size() : stop) - start;
        if (!n || (n == 1 && path[start] == '.')
            || (n == 2 && path.compare(start, n, "..") == 0))
            return false;
        if (stop == std::string::npos) return true;
        start = stop + 1;
    }
    return false;
}

bool safe_ancestor(const int fd, const bool last)
{
    struct stat s {};
    if (fstat(fd, &s) != 0 || !S_ISDIR(s.st_mode))
        return false;
    if (last)
        return s.st_uid == geteuid() && (s.st_mode & 07777) == 0700;
    // Root-owned and owner-owned ancestors are permitted, but not writable
    // by group/other; reject sticky /tmp and every symlink component.
    return (s.st_uid == 0 || s.st_uid == geteuid())
        && (s.st_mode & 0022) == 0;
}

std::optional<std::array<unsigned char, 32>> read_secure_root(
    const std::string& path)
{
    if (!canonical_path(path))
        return std::nullopt;

    Fd directory(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.get() < 0 || !safe_ancestor(directory.get(), false))
        return std::nullopt;

    std::size_t offset = 1;
    while (true) {
        const std::size_t slash = path.find('/', offset);
        if (slash == std::string::npos) break;
        const std::string component = path.substr(offset, slash - offset);
        Fd next(openat(directory.get(), component.c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0 || !safe_ancestor(next.get(), false))
            return std::nullopt;
        // Transfer the verified directory fd, never reopen by pathname.
        directory.reset(next.release());
        offset = slash + 1;
    }

    if (!safe_ancestor(directory.get(), true))
        return std::nullopt;
    const std::string filename = path.substr(offset);
    Fd file(openat(directory.get(), filename.c_str(),
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0)
        return std::nullopt;

    struct stat before {};
    if (fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode)
        || before.st_uid != geteuid()
        || (before.st_mode & 07777) != 0400
        || before.st_nlink != 1)
        return std::nullopt;

    const std::string prefix(local_goose_dialogue_responder_retry_root_pin_header);
    const std::size_t exact_bytes = prefix.size() + 64u + 1u;
    if (before.st_size != static_cast<off_t>(exact_bytes))
        return std::nullopt;
    std::string content(exact_bytes, '\0');
    std::size_t nread = 0;
    while (nread < exact_bytes) {
        const ssize_t n = read(file.get(), &content[nread], exact_bytes - nread);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt;
        nread += static_cast<std::size_t>(n);
    }
    char extra = 0;
    ssize_t n = -1;
    do { n = read(file.get(), &extra, 1); }
    while (n < 0 && errno == EINTR);
    if (n != 0) return std::nullopt;

    struct stat after {};
    if (fstat(file.get(), &after) != 0
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return std::nullopt;

    if (content.compare(0, prefix.size(), prefix) != 0
        || content.back() != '\n')
        return std::nullopt;
    const std::string key_hex = content.substr(prefix.size(), 64);
    if (!lower_hex(key_hex, 64)) return std::nullopt;
    std::array<unsigned char, 32> key {};
    bool nonzero = false;
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<unsigned char>(
            (decode(key_hex[2*i]) << 4u) | decode(key_hex[2*i+1]));
        nonzero = nonzero || key[i] != 0;
    }
    if (!nonzero) return std::nullopt;
    return key;
}

} // namespace

std::optional<LocalGooseDialogueResponderRetryRootPin>
load_local_goose_dialogue_responder_retry_root_pin(
    const std::string& trusted_absolute_path,
    const std::string& independently_enrolled_root_fingerprint)
{
    try {
        // The expected root fingerprint MUST come from a distinct trusted
        // enrollment source, never from the candidate file or Drive.
        if (!lower_hex(independently_enrolled_root_fingerprint, 64))
            return std::nullopt;

        const auto key = read_secure_root(trusted_absolute_path);
        if (!key) return std::nullopt;
        const std::string raw(reinterpret_cast<const char*>(key->data()), key->size());
        const std::string fingerprint =
            sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
        if (fingerprint != independently_enrolled_root_fingerprint)
            return std::nullopt;

        return LocalGooseDialogueResponderRetryRootPin(*key, fingerprint);
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace gaudere_agent
