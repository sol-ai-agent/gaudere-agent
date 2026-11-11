#include "LocalGooseDialogueResponderRetryRootOwnedAnchor.hpp"

#include "LocalGooseDialogueResponderRetryPinnedTrustPreflight.hpp"
#include "LocalGooseDialogueResponderRetryRootPin.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryRootOwnedPreflightCode;

class Fd {
public:
    explicit Fd(const int fd) noexcept : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }
private:
    int fd_;
};

bool lower_sha256(const std::string& value) noexcept
{
    if (value.size() != 64) return false;
    bool nonzero = false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

bool canonical_path(const std::string& value) noexcept
{
    if (value.size() < 3 || value.size() > 512 || value[0] != '/'
        || value.find('\0') != std::string::npos)
        return false;
    std::size_t start = 1;
    while (start < value.size()) {
        const auto slash = value.find('/', start);
        const auto n = (slash == std::string::npos ? value.size() : slash) - start;
        if (!n || (n == 1 && value[start] == '.')
            || (n == 2 && value.compare(start, n, "..") == 0))
            return false;
        if (slash == std::string::npos) return true;
        start = slash + 1;
    }
    return false;
}

bool trusted_directory(const int fd) noexcept
{
    struct stat s {};
    return fstat(fd, &s) == 0 && S_ISDIR(s.st_mode)
        && s.st_uid == 0 && (s.st_mode & 0022) == 0;
}

bool trusted_file(const int fd, struct stat& s) noexcept
{
    return fstat(fd, &s) == 0 && S_ISREG(s.st_mode)
        && s.st_uid == 0 && (s.st_mode & 07777) == 0644
        && s.st_nlink == 1 && s.st_size > 0 && s.st_size <= 1024;
}

std::optional<std::string> extract(const std::string& raw,
                                   std::size_t& offset, const char* name)
{
    const std::string prefix(name);
    if (raw.compare(offset, prefix.size(), prefix) != 0)
        return std::nullopt;
    const auto from = offset + prefix.size();
    const auto end = raw.find('\n', from);
    if (end == std::string::npos) return std::nullopt;
    offset = end + 1;
    return raw.substr(from, end - from);
}

std::optional<LocalGooseDialogueResponderRetryRootOwnedAnchorRecord>
parse(const std::string& raw)
{
    const std::string header(
        local_goose_dialogue_responder_retry_root_owned_anchor_header);
    if (raw.compare(0, header.size(), header) != 0
        || raw.find('\0') != std::string::npos)
        return std::nullopt;
    std::size_t position = header.size();
    auto path = extract(raw, position, "root_pin_path=");
    auto root = extract(raw, position, "root_fingerprint=");
    auto op = extract(raw, position, "operator_key_fingerprint=");
    auto epoch = extract(raw, position, "anchor_epoch=");
    if (!path || !root || !op || !epoch || position != raw.size()
        || !canonical_path(*path) || !lower_sha256(*root)
        || !lower_sha256(*op)
        || epoch->empty() || epoch->size() > 19
        || (epoch->size() > 1 && epoch->front() == '0'))
        return std::nullopt;

    std::uint64_t generation = 0;
    constexpr auto upper =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (const unsigned char c : *epoch) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = c - '0';
        if (digit > upper || generation > (upper - digit) / 10u)
            return std::nullopt;
        generation = generation * 10u + digit;
    }
    if (generation == 0) return std::nullopt;

    LocalGooseDialogueResponderRetryRootOwnedAnchorRecord result;
    result.root_pin_path = *path;
    result.root_fingerprint = *root;
    result.operator_key_fingerprint = *op;
    result.anchor_epoch = generation;
    return result;
}

} // namespace

std::optional<LocalGooseDialogueResponderRetryRootOwnedAnchorRecord>
load_local_goose_dialogue_responder_retry_root_owned_anchor()
{
    try {
        // An unprivileged runtime UID cannot create a root-owned trusted file.
        // Running this inspector as root is specifically not an enrollment gate.
        if (geteuid() == 0) return std::nullopt;

        // The exact pathname is compiled in: no request-controlled traversal.
        Fd root(open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (root.get() < 0 || !trusted_directory(root.get()))
            return std::nullopt;
        Fd etc(openat(root.get(), "etc",
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (etc.get() < 0 || !trusted_directory(etc.get()))
            return std::nullopt;
        Fd gaudere(openat(etc.get(), "gaudere",
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (gaudere.get() < 0 || !trusted_directory(gaudere.get()))
            return std::nullopt;
        Fd file(openat(gaudere.get(), "retry-operator-root-anchor.v1",
            O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        struct stat before {};
        if (file.get() < 0 || !trusted_file(file.get(), before))
            return std::nullopt;

        std::string raw(static_cast<std::size_t>(before.st_size), '\0');
        std::size_t offset = 0;
        while (offset < raw.size()) {
            const ssize_t n = read(file.get(), &raw[offset], raw.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return std::nullopt;
            offset += static_cast<std::size_t>(n);
        }
        char extra {};
        ssize_t n = -1;
        do { n = read(file.get(), &extra, 1); }
        while (n < 0 && errno == EINTR);
        if (n != 0) return std::nullopt;

        struct stat after {};
        if (!trusted_file(file.get(), after)
            || before.st_dev != after.st_dev
            || before.st_ino != after.st_ino
            || before.st_size != after.st_size
            || before.st_mtim.tv_sec != after.st_mtim.tv_sec
            || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
            || before.st_ctim.tv_sec != after.st_ctim.tv_sec
            || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            return std::nullopt;
        return parse(raw);
    } catch (...) {
        return std::nullopt;
    }
}

Code inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
    const LocalGooseDialogueResponderRetryTrustFloorLedger& existing_read_only_floor,
    const std::string& candidate_signed_snapshot,
    const std::int64_t trusted_now_ms)
{
    const auto anchor = load_local_goose_dialogue_responder_retry_root_owned_anchor();
    if (!anchor)
        return Code::source_missing_or_untrusted_no_authority;
    const auto pinned_root = load_local_goose_dialogue_responder_retry_root_pin(
        anchor->root_pin_path, anchor->root_fingerprint);
    if (!pinned_root)
        return Code::root_pin_invalid_no_authority;
    const auto inspected = inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
        *pinned_root, existing_read_only_floor,
        candidate_signed_snapshot, anchor->operator_key_fingerprint,
        trusted_now_ms);
    using CoreCode = LocalGooseDialogueResponderRetryPinnedTrustPreflightCode;
    switch (inspected) {
        case CoreCode::consistent_active_no_authority:
            return Code::consistent_active_no_authority;
        case CoreCode::consistent_revoked_no_authority:
            return Code::consistent_revoked_no_authority;
        default:
            return Code::signed_policy_or_floor_invalid_no_authority;
    }
}

} // namespace gaudere_agent
