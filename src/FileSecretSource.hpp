#ifndef GAUDERE_AGENT_FILE_SECRET_SOURCE_HPP
#define GAUDERE_AGENT_FILE_SECRET_SOURCE_HPP

#include "SecretSource.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace gaudere_agent {

/** Read-only secret source rooted at one already-authorized directory.
 *
 * Names are single path components. Files are opened with openat()+O_NOFOLLOW,
 * must be regular files, must not grant any group/other permissions, and are
 * bounded before being exposed as SecretValue.
 */
class FileSecretSource final : public SecretSource {
public:
    explicit FileSecretSource(std::string directory = "/run/secrets",
                              std::size_t max_secret_bytes = 16 * 1024);
    ~FileSecretSource() override;

    FileSecretSource(const FileSecretSource&) = delete;
    FileSecretSource& operator=(const FileSecretSource&) = delete;
    FileSecretSource(FileSecretSource&&) = delete;
    FileSecretSource& operator=(FileSecretSource&&) = delete;

    [[nodiscard]] std::optional<SecretValue> load(
        std::string_view name) override;

private:
    [[nodiscard]] static bool valid_name(std::string_view name) noexcept;

    std::string directory_;
    std::size_t max_secret_bytes_;
    int directory_fd_ = -1;
};

} // namespace gaudere_agent

#endif
