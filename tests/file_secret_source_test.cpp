#include "FileSecretSource.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <utility>

namespace {

using gaudere_agent::FileSecretSource;
using gaudere_agent::SecretValue;

static_assert(!std::is_copy_constructible_v<SecretValue>);
static_assert(!std::is_copy_assignable_v<SecretValue>);
static_assert(std::is_move_constructible_v<SecretValue>);
static_assert(std::is_move_assignable_v<SecretValue>);

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Exception, typename Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        std::forward<Function>(function)();
        expect(false, message);
    } catch (const Exception&) {
        // Expected.
    } catch (...) {
        expect(false, message + " (wrong exception type)");
    }
}

struct TemporaryDirectory {
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-secret-source-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

void write_secret(const std::filesystem::path& path,
                  const std::string& content,
                  const mode_t mode = 0400)
{
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create test secret");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    if (::chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error("cannot chmod test secret");
    }
}

void test_exact_read_and_move_only_value()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "provider-key", "alpha-secret\n");

    FileSecretSource source(directory.path.string());
    auto secret = source.load("provider-key");
    expect(secret.has_value(), "existing secret is loaded");
    expect(secret && secret->view() == "alpha-secret\n",
           "secret bytes are returned exactly without newline trimming");

    SecretValue moved = std::move(*secret);
    expect(moved.view() == "alpha-secret\n", "move-only secret preserves bytes");
}

void test_missing_secret()
{
    TemporaryDirectory directory;
    FileSecretSource source(directory.path.string());
    expect(!source.load("missing"), "missing secret is distinct from an invalid secret");
}

void test_name_confinement()
{
    TemporaryDirectory directory;
    FileSecretSource source(directory.path.string());

    expect_throw<std::invalid_argument>([&] { static_cast<void>(source.load("")); },
                                        "empty secret name is rejected");
    expect_throw<std::invalid_argument>([&] { static_cast<void>(source.load("..")); },
                                        "parent secret name is rejected");
    expect_throw<std::invalid_argument>([&] { static_cast<void>(source.load("../key")); },
                                        "parent traversal is rejected");
    expect_throw<std::invalid_argument>([&] { static_cast<void>(source.load("sub/key")); },
                                        "subdirectory traversal is rejected");
    expect_throw<std::invalid_argument>([&] { static_cast<void>(source.load("key space")); },
                                        "unexpected secret-name characters are rejected");
}

void test_symlink_rejected()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "actual", "secret");
    std::filesystem::create_symlink("actual", directory.path / "alias");

    FileSecretSource source(directory.path.string());
    expect_throw<std::runtime_error>([&] { static_cast<void>(source.load("alias")); },
                                     "secret symlink is rejected by O_NOFOLLOW");
}

void test_broad_permissions_rejected()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "public-key", "secret", 0444);

    FileSecretSource source(directory.path.string());
    expect_throw<std::runtime_error>(
        [&] { static_cast<void>(source.load("public-key")); },
        "group/world-readable secret is rejected");
}

void test_size_limit()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "too-large", "12345");

    FileSecretSource source(directory.path.string(), 4);
    expect_throw<std::runtime_error>([&] { static_cast<void>(source.load("too-large")); },
                                     "oversized secret is rejected before exposure");
}

void test_secret_directory_symlink_rejected()
{
    TemporaryDirectory parent;
    std::filesystem::create_directory(parent.path / "real");
    std::filesystem::create_directory_symlink("real", parent.path / "alias");

    expect_throw<std::runtime_error>(
        [&] { FileSecretSource source((parent.path / "alias").string()); },
        "secret root directory symlink is rejected");
}

} // namespace

int main()
{
    test_exact_read_and_move_only_value();
    test_missing_secret();
    test_name_confinement();
    test_symlink_rejected();
    test_broad_permissions_rejected();
    test_size_limit();
    test_secret_directory_symlink_rejected();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All file secret source tests passed\n";
    return 0;
}
