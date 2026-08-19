#ifndef GAUDERE_AGENT_SECRET_SOURCE_HPP
#define GAUDERE_AGENT_SECRET_SOURCE_HPP

#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace gaudere_agent {

/** Move-only sensitive byte buffer.
 *
 * The value has no stream/string conversion. Its owned bytes are overwritten on
 * destruction as a best-effort reduction of secret lifetime in process memory.
 */
class SecretValue {
public:
    explicit SecretValue(std::vector<char> bytes) noexcept
        : bytes_(std::move(bytes))
    {
    }

    ~SecretValue() { wipe(); }

    SecretValue(const SecretValue&) = delete;
    SecretValue& operator=(const SecretValue&) = delete;

    SecretValue(SecretValue&& other) noexcept
        : bytes_(std::move(other.bytes_))
    {
        other.wipe();
    }

    SecretValue& operator=(SecretValue&& other) noexcept
    {
        if (this != &other) {
            wipe();
            bytes_ = std::move(other.bytes_);
            other.wipe();
        }
        return *this;
    }

    [[nodiscard]] std::string_view view() const noexcept
    {
        return std::string_view(bytes_.data(), bytes_.size());
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

private:
    void wipe() noexcept
    {
        volatile char* data = bytes_.data();
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            data[index] = 0;
        }
        bytes_.clear();
    }

    std::vector<char> bytes_;
};

/** Named secret lookup boundary.
 *
 * Missing secrets return std::nullopt. Invalid names, insecure storage, I/O
 * failures, or oversized values are errors and must not be silently treated as
 * an absent credential.
 */
class SecretSource {
public:
    virtual ~SecretSource() = default;

    [[nodiscard]] virtual std::optional<SecretValue> load(
        std::string_view name) = 0;
};

} // namespace gaudere_agent

#endif
