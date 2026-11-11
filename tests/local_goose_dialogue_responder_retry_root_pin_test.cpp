#include "LocalGooseDialogueResponderRetryRootPin.hpp"
#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <type_traits>

namespace {

using gaudere_agent::LocalGooseDialogueResponderRetryRootPin;
using gaudere_agent::local_goose_dialogue_responder_retry_root_pin_header;
using gaudere_agent::load_local_goose_dialogue_responder_retry_root_pin;
using gaudere_agent::sha256_hex;

static_assert(!std::is_default_constructible<LocalGooseDialogueResponderRetryRootPin>::value,
    "An unvalidated root key cannot create its own trusted handle");
static_assert(!std::is_constructible<LocalGooseDialogueResponderRetryRootPin,
    std::array<unsigned char, 32>, std::string>::value,
    "An arbitrary caller cannot enroll its own root key");

class RootKey {
public:
    std::array<unsigned char, 32> pub {};
    explicit RootKey(const unsigned char seed_byte)
    {
        std::array<unsigned char, 32> seed {};
        seed.fill(seed_byte);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
            EVP_PKEY_new_raw_private_key(
                EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()),
            EVP_PKEY_free);
        assert(key);
        std::size_t n = pub.size();
        assert(EVP_PKEY_get_raw_public_key(key.get(), pub.data(), &n) == 1);
        assert(n == pub.size());
    }
};

struct Temporary {
    explicit Temporary(const std::string& tag)
    {
        dir = std::filesystem::temp_directory_path()
            / ("gaudere-root-pin-" + tag + "-"
               + std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch().count()));
        assert(std::filesystem::create_directory(dir));
        assert(chmod(dir.string().c_str(), 0700) == 0);
        path = dir / "root.pin";
    }
    ~Temporary()
    {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    }
    std::filesystem::path dir, path;
};

std::string to_hex(const std::array<unsigned char, 32>& key)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char c : key) {
        result.push_back(digits[c >> 4u]);
        result.push_back(digits[c & 15u]);
    }
    return result;
}

std::string fingerprint(const RootKey& key)
{
    const std::string raw(reinterpret_cast<const char*>(key.pub.data()), key.pub.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

std::string canonical_content(const RootKey& key)
{
    return std::string(local_goose_dialogue_responder_retry_root_pin_header)
        + to_hex(key.pub) + "\n";
}

void rewrite(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output.good());
    assert(chmod(path.string().c_str(), 0400) == 0);
}

bool accepts(const std::filesystem::path& path, const RootKey& key)
{
    auto inspected = load_local_goose_dialogue_responder_retry_root_pin(
        path.string(), fingerprint(key));
    return inspected && inspected->public_key() == key.pub
        && inspected->fingerprint() == fingerprint(key);
}

void test_strict_owner_only_root_file()
{
    RootKey root(0x2a), other(0x7b);
    Temporary t("valid");
    assert(!accepts(t.path, root));
    rewrite(t.path, canonical_content(root));
    assert(accepts(t.path, root));
    assert(accepts(t.path, root));  // no enrollment, no mutation

    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.path.string(), fingerprint(other)));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.path.string(), std::string(64, 'f')));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.path.string(), "not-a-fingerprint"));

    assert(chmod(t.path.string().c_str(), 0644) == 0);
    assert(!accepts(t.path, root));
    assert(chmod(t.path.string().c_str(), 0600) == 0);
    assert(!accepts(t.path, root));
    assert(chmod(t.path.string().c_str(), 0400) == 0);
    assert(accepts(t.path, root));

    const auto alias = t.dir / "root-symlink";
    std::filesystem::create_symlink(t.path, alias);
    assert(!accepts(alias, root));
    const auto second = t.dir / "root-hardlink";
    std::filesystem::create_hard_link(t.path, second);
    assert(!accepts(t.path, root));
    assert(!accepts(second, root));
    std::filesystem::remove(second);
    assert(accepts(t.path, root));
}

void test_path_components_and_parent_permissions()
{
    RootKey root(0x2b);
    Temporary t("paths");
    rewrite(t.path, canonical_content(root));
    assert(accepts(t.path, root));

    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        "relative/root.pin", fingerprint(root)));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.dir.string() + "/./root.pin", fingerprint(root)));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.dir.string() + "//root.pin", fingerprint(root)));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.dir.string() + "/x/../root.pin", fingerprint(root)));
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        t.path.string() + std::string("\0garbage", 8), fingerprint(root)));

    assert(chmod(t.dir.string().c_str(), 0755) == 0);
    assert(!accepts(t.path, root));
    assert(chmod(t.dir.string().c_str(), 0770) == 0);
    assert(!accepts(t.path, root));
    assert(chmod(t.dir.string().c_str(), 0700) == 0);

    // The final directory may be private while an intermediate component
    // is a symlink. The scanner must NOT resolve it.
    const auto nested = t.dir / "trusted";
    std::filesystem::create_directory(nested);
    assert(chmod(nested.string().c_str(), 0700) == 0);
    const auto nested_pin = nested / "root.pin";
    rewrite(nested_pin, canonical_content(root));
    assert(accepts(nested_pin, root));
    const auto alias = t.dir / "linked-parent";
    std::filesystem::create_directory_symlink(nested, alias);
    assert(!accepts(alias / "root.pin", root));

    // Intermediate owner-writable by other users (not root-owned /tmp)
    // is never acceptable, even if the immediate parent is 0700.
    assert(chmod(t.dir.string().c_str(), 0777) == 0);
    assert(!accepts(nested_pin, root));
    assert(chmod(t.dir.string().c_str(), 0700) == 0);
    assert(accepts(nested_pin, root));
}

void test_canonical_encoding_and_rotation()
{
    RootKey root(0x39), attacker(0x40);
    Temporary t("encoding");
    rewrite(t.path, canonical_content(root));
    assert(accepts(t.path, root));

    const auto reject = [&](const std::string& bytes) {
        assert(chmod(t.path.string().c_str(), 0600) == 0);
        rewrite(t.path, bytes);
        assert(!accepts(t.path, root));
    };
    const auto canonical = canonical_content(root);
    reject(canonical + "\n");
    reject(canonical.substr(0, canonical.size() - 1));
    reject(std::string("root=untrusted\n") + canonical);
    reject(std::string(local_goose_dialogue_responder_retry_root_pin_header)
           + std::string(64, '0') + "\n");
    std::string uppercase = canonical;
    uppercase[uppercase.size() - 2] = 'F';
    reject(uppercase);

    // An attacker replacing the candidate file with another valid public
    // key cannot change the independent trusted fingerprint.
    assert(chmod(t.path.string().c_str(), 0600) == 0);
    rewrite(t.path, canonical_content(attacker));
    assert(!accepts(t.path, root));
    assert(accepts(t.path, attacker)); // only with a different OUT-OF-BAND anchor

    assert(chmod(t.path.string().c_str(), 0600) == 0);
    rewrite(t.path, canonical);
    assert(accepts(t.path, root));
}

} // namespace

int main()
{
    test_strict_owner_only_root_file();
    test_path_components_and_parent_permissions();
    test_canonical_encoding_and_rotation();
    return 0;
}
