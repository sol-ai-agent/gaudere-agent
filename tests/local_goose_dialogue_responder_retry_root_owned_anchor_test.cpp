#include "LocalGooseDialogueResponderRetryRootOwnedAnchor.hpp"
#include "LocalGooseDialogueResponderRetryVerifiedRevocation.hpp"
#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"
#include "LocalGooseDialogueResponderRetryRootPin.hpp"
#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace gaudere_agent;
using Preflight = LocalGooseDialogueResponderRetryRootOwnedPreflightCode;
using Floor = LocalGooseDialogueResponderRetryTrustFloorLedger;
using Mode = LocalGooseDialogueResponderRetryTrustFloorOpenMode;
using Commit = LocalGooseDialogueResponderRetryRevocationCommitCode;

struct Key {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> priv {
        nullptr, EVP_PKEY_free
    };
    explicit Key(const unsigned char b)
    {
        // Fixed synthetic CI-only material. NOT an actual enrolled key.
        std::array<unsigned char, 32> seed {};
        seed.fill(b);
        priv.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(priv);
        std::size_t len = pub.size();
        assert(EVP_PKEY_get_raw_public_key(priv.get(), pub.data(), &len) == 1);
        assert(len == pub.size());
    }
};

std::string hex(const unsigned char* bytes, std::size_t n)
{
    constexpr char alphabet[] = "0123456789abcdef";
    std::string value;
    value.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        value.push_back(alphabet[bytes[i] >> 4]);
        value.push_back(alphabet[bytes[i] & 15]);
    }
    return value;
}

std::string fingerprint(const Key& root)
{
    const std::string raw(
        reinterpret_cast<const char*>(root.pub.data()), root.pub.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

std::string sign(Key& key, const std::string& payload)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(ctx && EVP_DigestSignInit(
        ctx.get(), nullptr, nullptr, nullptr, key.priv.get()) == 1);
    std::array<unsigned char, 64> signature {};
    std::size_t len = signature.size();
    assert(EVP_DigestSign(ctx.get(), signature.data(), &len,
        reinterpret_cast<const unsigned char*>(payload.data()),
        payload.size()) == 1);
    assert(len == signature.size());
    return hex(signature.data(), len);
}

std::string policy(Key& key, const std::string& op,
                   const char* status, const unsigned generation)
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_trust_snapshot_header)
        + "key_fingerprint=" + op + "\n"
        + "status=" + status + "\n"
        + "generation=" + std::to_string(generation) + "\n"
        + "not_before_ms=1000\n"
        + "not_after_ms=30000\n";
    return body + "signature_hex="
        + sign(key, "gaudere-retry-operator-trust-snapshot-signature.v1|" + body)
        + "\n";
}

void write_text(const std::string& path, const std::string& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output.good());
}

std::string manifest(const std::string& pin, const std::string& root,
                     const std::string& op)
{
    return std::string(local_goose_dialogue_responder_retry_root_owned_anchor_header)
        + "root_pin_path=" + pin + "\n"
        + "root_fingerprint=" + root + "\n"
        + "operator_key_fingerprint=" + op + "\n"
        + "anchor_epoch=1\n";
}

void child_expect_missing()
{
    assert(!load_local_goose_dialogue_responder_retry_root_owned_anchor());
}

void child_run_positive(const std::string& pin_path,
                        const std::string& db_path)
{
    Key root(0x43), foreign(0x49);
    const std::string expected_op(64, 'b');
    const auto manifest_record =
        load_local_goose_dialogue_responder_retry_root_owned_anchor();
    assert(manifest_record);
    assert(manifest_record->root_pin_path == pin_path);
    assert(manifest_record->root_fingerprint == fingerprint(root));
    assert(manifest_record->operator_key_fingerprint == expected_op);
    assert(manifest_record->anchor_epoch == 1);

    // Caller can create an attacker-controlled candidate file, but cannot
    // select the already installed root-owned manifest's trust fingerprint.
    write_text(pin_path,
        std::string(local_goose_dialogue_responder_retry_root_pin_header)
        + hex(root.pub.data(), root.pub.size()) + "\n");
    assert(chmod(pin_path.c_str(), 0400) == 0);
    const auto root_pin = load_local_goose_dialogue_responder_retry_root_pin(
        pin_path, manifest_record->root_fingerprint);
    assert(root_pin);

    const auto active1 = policy(root, expected_op, "active", 1);
    const auto revoke8 = policy(root, expected_op, "revoked", 8);
    const auto newer_active9 = policy(root, expected_op, "active", 9);
    const auto foreign_active = policy(foreign, expected_op, "active", 1);
    const auto wrong_operator = policy(root, std::string(64, 'a'), "active", 1);
    Floor bootstrap(db_path, Mode::create_new_isolated,
                    manifest_record->root_fingerprint);
    assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
        bootstrap, active1, 5000)
        == Preflight::signed_policy_or_floor_invalid_no_authority);
    {
        Floor floor(db_path, Mode::existing_read_only);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, active1, 5000)
            == Preflight::consistent_active_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, revoke8, 5000)
            == Preflight::consistent_revoked_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, foreign_active, 5000)
            == Preflight::signed_policy_or_floor_invalid_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, wrong_operator, 5000)
            == Preflight::signed_policy_or_floor_invalid_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, active1, 30000)
            == Preflight::signed_policy_or_floor_invalid_no_authority);
        auto forged = active1;
        const auto pos = forged.find("status=active");
        assert(pos != std::string::npos);
        forged[pos + 7] = 'z';
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            floor, forged, 5000)
            == Preflight::signed_policy_or_floor_invalid_no_authority);
    }

    assert(commit_local_goose_dialogue_responder_retry_verified_revocation(
        bootstrap, revoke8, *root_pin, expected_op, 5000)
        == Commit::advanced_no_authority);
    {
        Floor restarted(db_path, Mode::existing_read_only);
        assert(restarted.inspect(fingerprint(root))->minimum_generation == 8);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            restarted, active1, 5000)
            == Preflight::signed_policy_or_floor_invalid_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            restarted, revoke8, 5000)
            == Preflight::consistent_revoked_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_root_owned_preflight(
            restarted, newer_active9, 5000)
            == Preflight::consistent_active_no_authority);
    }
}

template<class Function>
void as_unprivileged(Function fn)
{
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setgid(65534) == 0);
        assert(setuid(65534) == 0);
        assert(geteuid() == 65534);
        fn();
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void test_root_owned_fixed_source_in_disposable_fedora_container()
{
    // The Fedora CI container MUST be disposable. Never run on a real host.
    assert(geteuid() == 0);
    const std::string manifest_path =
        local_goose_dialogue_responder_retry_root_owned_anchor_path;
    const auto tempdir = std::filesystem::temp_directory_path() /
        ("gaudere-anchor-ci-" + std::to_string(getpid()));
    assert(std::filesystem::create_directory(tempdir));
    assert(chown(tempdir.c_str(), 65534, 65534) == 0);
    assert(chmod(tempdir.c_str(), 0700) == 0);
    const auto pin = (tempdir / "root.pin").string();
    const auto db = (tempdir / "floor.sqlite").string();

    // No preexisting operator state may be overwritten in CI.
    assert(mkdir("/etc/gaudere", 0755) == 0);
    as_unprivileged(child_expect_missing);

    Key root(0x43);
    const std::string expected_op(64, 'b');
    write_text(manifest_path, manifest(pin, fingerprint(root), expected_op));
    assert(chmod(manifest_path.c_str(), 0666) == 0);
    as_unprivileged(child_expect_missing); // group/world-write -> deny
    assert(chmod(manifest_path.c_str(), 0644) == 0);

    // Parent directory cannot be writable by the runtime UID.
    assert(chmod("/etc/gaudere", 0777) == 0);
    as_unprivileged(child_expect_missing);
    assert(chmod("/etc/gaudere", 0755) == 0);

    // Canonical file content cannot silently self-enroll from a symlink.
    assert(unlink(manifest_path.c_str()) == 0);
    assert(symlink("/etc/passwd", manifest_path.c_str()) == 0);
    as_unprivileged(child_expect_missing);
    assert(unlink(manifest_path.c_str()) == 0);
    write_text(manifest_path, manifest(pin, fingerprint(root), expected_op));
    assert(chmod(manifest_path.c_str(), 0644) == 0);

    // A malformed or self-signed candidate fingerprint is not imported.
    write_text(manifest_path,
        manifest(pin, std::string(64, '0'), expected_op));
    as_unprivileged(child_expect_missing);
    write_text(manifest_path, manifest(pin, fingerprint(root), expected_op));
    assert(chmod(manifest_path.c_str(), 0644) == 0);
    as_unprivileged([&] { child_run_positive(pin, db); });

    assert(unlink(manifest_path.c_str()) == 0);
    assert(rmdir("/etc/gaudere") == 0);
    std::filesystem::remove_all(tempdir);
}

} // namespace

int main()
{
    test_root_owned_fixed_source_in_disposable_fedora_container();
    return 0;
}
