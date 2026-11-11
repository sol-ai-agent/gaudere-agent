#include "LocalGooseDialogueResponderRetryPinnedTrustPreflight.hpp"
#include "LocalGooseDialogueResponderRetryVerifiedRevocation.hpp"
#include "LocalGooseDialogueResponderRetryOperatorTrustSnapshot.hpp"
#include "Sha256.hpp"

#include <openssl/evp.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <sys/stat.h>

namespace {

using namespace gaudere_agent;
using Code = LocalGooseDialogueResponderRetryPinnedTrustPreflightCode;
using Ledger = LocalGooseDialogueResponderRetryTrustFloorLedger;
using Mode = LocalGooseDialogueResponderRetryTrustFloorOpenMode;
using Commit = LocalGooseDialogueResponderRetryRevocationCommitCode;

struct SyntheticRoot {
    std::array<unsigned char, 32> pub {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key {
        nullptr, EVP_PKEY_free
    };
    explicit SyntheticRoot(const unsigned char seed_byte)
    {
        // Synthetic fixed test seed only. Never an enrolled production key.
        std::array<unsigned char, 32> seed {};
        seed.fill(seed_byte);
        private_key.reset(EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
        assert(private_key);
        std::size_t size = pub.size();
        assert(EVP_PKEY_get_raw_public_key(
            private_key.get(), pub.data(), &size) == 1);
        assert(size == pub.size());
    }
};

std::string hex(const unsigned char* bytes, const std::size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

std::string root_fingerprint(const SyntheticRoot& root)
{
    const std::string raw(reinterpret_cast<const char*>(root.pub.data()),
                          root.pub.size());
    return sha256_hex("gaudere-retry-operator-root-ed25519-key-v1|" + raw);
}

std::string sign(SyntheticRoot& root, const std::string& exact_message)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    assert(context && EVP_DigestSignInit(
        context.get(), nullptr, nullptr, nullptr, root.private_key.get()) == 1);
    std::array<unsigned char, 64> signature {};
    std::size_t size = signature.size();
    assert(EVP_DigestSign(context.get(), signature.data(), &size,
        reinterpret_cast<const unsigned char*>(exact_message.data()),
        exact_message.size()) == 1);
    assert(size == signature.size());
    return hex(signature.data(), size);
}

std::string signed_policy(SyntheticRoot& root, const std::string& operator_fp,
                          const std::string& status, const unsigned generation)
{
    const std::string body =
        std::string(local_goose_dialogue_responder_retry_trust_snapshot_header)
        + "key_fingerprint=" + operator_fp + "\n"
        + "status=" + status + "\n"
        + "generation=" + std::to_string(generation) + "\n"
        + "not_before_ms=1000\n"
        + "not_after_ms=30000\n";
    return body + "signature_hex="
        + sign(root, "gaudere-retry-operator-trust-snapshot-signature.v1|" + body)
        + "\n";
}

struct Temp {
    std::filesystem::path dir, root, foreign_root, db;
    Temp()
    {
        dir = std::filesystem::temp_directory_path() /
            ("gaudere-pinned-trust-preflight-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        assert(std::filesystem::create_directory(dir));
        assert(chmod(dir.c_str(), 0700) == 0);
        root = dir / "root.pin";
        foreign_root = dir / "foreign-root.pin";
        db = dir / "floor.db";
    }
    ~Temp()
    {
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }
};

void write_root(const std::filesystem::path& path, const SyntheticRoot& key)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output.good());
    const auto bytes =
        std::string(local_goose_dialogue_responder_retry_root_pin_header)
        + hex(key.pub.data(), key.pub.size()) + "\n";
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    assert(output.good());
    assert(chmod(path.c_str(), 0400) == 0);
}

void test_protected_root_signed_policy_and_read_only_floor()
{
    Temp temp;
    SyntheticRoot root(0x37), foreign(0x73);
    const std::string operator_fp(64, 'a');
    write_root(temp.root, root);
    write_root(temp.foreign_root, foreign);
    const auto pinned = load_local_goose_dialogue_responder_retry_root_pin(
        temp.root.string(), root_fingerprint(root));
    assert(pinned);
    assert(!load_local_goose_dialogue_responder_retry_root_pin(
        temp.root.string(), root_fingerprint(foreign)));
    const auto foreign_pin = load_local_goose_dialogue_responder_retry_root_pin(
        temp.foreign_root.string(), root_fingerprint(foreign));
    assert(foreign_pin);

    const auto active1 = signed_policy(root, operator_fp, "active", 1);
    const auto revoked8 = signed_policy(root, operator_fp, "revoked", 8);
    const auto active9 = signed_policy(root, operator_fp, "active", 9);
    {
        Ledger bootstrap(temp.db.string(), Mode::create_new_isolated,
                         root_fingerprint(root));
        assert(bootstrap.inspect(root_fingerprint(root))->minimum_generation == 1);
        assert(inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
            *pinned, bootstrap, active1, operator_fp, 5000)
            == Code::mutable_floor_forbidden);
    }
    {
        Ledger read_only(temp.db.string(), Mode::existing_read_only);
        assert(read_only.is_read_only());
        const auto inspect = [&](const std::string& data,
                                 const std::string& expected, const std::int64_t now) {
            return inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
                *pinned, read_only, data, expected, now);
        };
        assert(inspect(active1, operator_fp, 5000)
            == Code::consistent_active_no_authority);
        assert(inspect(active1, operator_fp, 5000)
            == Code::consistent_active_no_authority);
        assert(inspect(revoked8, operator_fp, 5000)
            == Code::consistent_revoked_no_authority);
        assert(inspect(active1, "malformed", 5000)
            == Code::invalid_expected_operator_key);
        assert(inspect(active1, std::string(64, 'b'), 5000)
            == Code::signed_policy_wrong_operator_key);
        assert(inspect(active1, operator_fp, 30000)
            == Code::invalid_signed_policy_or_time);
        auto tampered = active1;
        tampered[tampered.find("status=active") + 7] = 'z';
        assert(inspect(tampered, operator_fp, 5000)
            == Code::invalid_signed_policy_or_time);
        assert(inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
            *foreign_pin, read_only, active1, operator_fp, 5000)
            == Code::floor_unavailable_or_wrong_root);
        assert(read_only.inspect(root_fingerprint(root))->minimum_generation == 1);
    }

    // A separate #285 root-signed revocation advances the test-only floor.
    // The read-only preflight itself has no mutation route.
    {
        Ledger writer(temp.db.string(), Mode::existing_writer);
        assert(commit_local_goose_dialogue_responder_retry_verified_revocation(
            writer, revoked8, *pinned, operator_fp, 5000)
            == Commit::advanced_no_authority);
        assert(writer.inspect(root_fingerprint(root))->minimum_generation == 8);
    }
    {
        Ledger restarted(temp.db.string(), Mode::existing_read_only);
        assert(restarted.inspect(root_fingerprint(root))->minimum_generation == 8);
        assert(inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
            *pinned, restarted, active1, operator_fp, 5000)
            == Code::signed_policy_below_floor);
        assert(inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
            *pinned, restarted, revoked8, operator_fp, 5000)
            == Code::consistent_revoked_no_authority);
        assert(inspect_local_goose_dialogue_responder_retry_pinned_trust_preflight(
            *pinned, restarted, active9, operator_fp, 5000)
            == Code::consistent_active_no_authority);
        assert(restarted.inspect(root_fingerprint(root))->minimum_generation == 8);
    }
}

} // namespace

int main()
{
    test_protected_root_signed_policy_and_read_only_floor();
    return 0;
}
