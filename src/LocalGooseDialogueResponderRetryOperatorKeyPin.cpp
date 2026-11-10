#include "LocalGooseDialogueResponderRetryOperatorKeyPin.hpp"

#include "Sha256.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace gaudere_agent {
namespace {

using Code = LocalGooseDialogueResponderRetryPinnedSignatureCode;

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

bool lower_hex(const std::string& s, const std::size_t length) noexcept
{
    if (s.size() != length) return false;
    for (const unsigned char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}

unsigned char hex_digit(const char c) noexcept
{
    return static_cast<unsigned char>(
        c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10);
}

std::optional<std::array<unsigned char, 32>> read_owner_pin(
    const std::string& path)
{
    if (path.empty() || path.front() != '/' || path.size() > 4096
        || path.find('\0') != std::string::npos)
        return std::nullopt;

    const Fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.get() < 0) return std::nullopt;

    struct stat before {};
    if (fstat(fd.get(), &before) != 0
        || !S_ISREG(before.st_mode)
        || before.st_uid != geteuid()
        || (before.st_mode & 0777) != 0400
        || before.st_nlink != 1)
        return std::nullopt;

    const std::string prefix(local_goose_dialogue_responder_retry_key_pin_header);
    const std::size_t expected_length = prefix.size() + 64u + 1u;
    if (before.st_size != static_cast<off_t>(expected_length))
        return std::nullopt;

    std::string content(expected_length, '\0');
    std::size_t done = 0;
    while (done < expected_length) {
        const ssize_t n = read(fd.get(), &content[done], expected_length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt;
        done += static_cast<std::size_t>(n);
    }
    char extra = '\0';
    ssize_t n = 0;
    do { n = read(fd.get(), &extra, 1); }
    while (n < 0 && errno == EINTR);
    if (n != 0) return std::nullopt;

    struct stat after {};
    if (fstat(fd.get(), &after) != 0
        || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino
        || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return std::nullopt;

    if (content.compare(0, prefix.size(), prefix) != 0
        || content.back() != '\n')
        return std::nullopt;
    const std::string pub_hex = content.substr(prefix.size(), 64);
    if (!lower_hex(pub_hex, 64)) return std::nullopt;
    std::array<unsigned char, 32> pub {};
    bool nonzero = false;
    for (std::size_t i = 0; i < pub.size(); ++i) {
        pub[i] = static_cast<unsigned char>(
            (hex_digit(pub_hex[2*i]) << 4u) | hex_digit(pub_hex[2*i+1]));
        nonzero = nonzero || pub[i] != 0;
    }
    if (!nonzero) return std::nullopt;
    return pub;
}

std::string fingerprint(const std::array<unsigned char, 32>& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.data()), key.size());
    return sha256_hex("gaudere-retry-operator-ed25519-key-v1|" + raw);
}

} // namespace

Code inspect_local_goose_dialogue_responder_retry_pinned_signature(
    const std::string& trusted_absolute_pin_path,
    const std::string& independently_trusted_key_fingerprint,
    const LocalGooseDialogueResponderRetryExactMessageReview& review,
    const LocalGooseDialogueResponderRetryGrantProposal& proposal,
    const LocalGooseDialogueResponderRetryScope& scope,
    const LocalGooseDialogueResponderRetryOperatorChallenge& challenge,
    const LocalGooseDialogueResponderRetrySignedExactMessageProof& signed_proof,
    const std::int64_t now_ms)
{
    try {
        // This independent fingerprint is a REQUIRED external trust anchor:
        // the proof, request and pin file must never supply or replace it.
        if (!lower_hex(independently_trusted_key_fingerprint, 64))
            return Code::trusted_fingerprint_invalid_or_mismatch;
        const auto key = read_owner_pin(trusted_absolute_pin_path);
        if (!key) return Code::key_pin_unavailable_or_unsafe;
        if (fingerprint(*key) != independently_trusted_key_fingerprint)
            return Code::trusted_fingerprint_invalid_or_mismatch;

        // This only checks the signature and its context. It neither proves
        // actual human presence nor grants permission to run a retry.
        const auto inspected =
            inspect_local_goose_dialogue_responder_retry_exact_message_signature(
                review, proposal, scope, challenge, signed_proof,
                *key, "bertrand", now_ms);
        return inspected
                    == LocalGooseDialogueResponderRetryExactMessageProofCode::
                        cryptographically_consistent_no_authority
            ? Code::cryptographically_consistent_no_authority
            : Code::signed_message_invalid_or_expired;
    } catch (...) {
        return Code::key_pin_unavailable_or_unsafe;
    }
}

} // namespace gaudere_agent
