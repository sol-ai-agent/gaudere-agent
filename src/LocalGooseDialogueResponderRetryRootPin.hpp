#ifndef GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ROOT_PIN_HPP
#define GAUDERE_AGENT_LOCAL_GOOSE_DIALOGUE_RESPONDER_RETRY_ROOT_PIN_HPP

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace gaudere_agent {

inline constexpr const char* local_goose_dialogue_responder_retry_root_pin_header =
    "gaudere-retry-operator-root-ed25519-pin.v1\n"
    "operator_id=bertrand\n"
    "public_key_hex=";

// Non-default-constructible and without a public key/fingerprint setter:
// only the strict protected-file reader below may manufacture a handle.
// Copy/move is permitted after successful verification. This is NOT
// authorization; the expected fingerprint must be chosen independently
// and protected by a future real operator enrollment controller.
class LocalGooseDialogueResponderRetryRootPin {
public:
    LocalGooseDialogueResponderRetryRootPin(
        const LocalGooseDialogueResponderRetryRootPin&) = default;
    LocalGooseDialogueResponderRetryRootPin(
        LocalGooseDialogueResponderRetryRootPin&&) = default;

    [[nodiscard]] const std::array<unsigned char, 32>& public_key() const noexcept
    {
        return public_key_;
    }
    [[nodiscard]] const std::string& fingerprint() const noexcept
    {
        return fingerprint_;
    }

private:
    friend std::optional<LocalGooseDialogueResponderRetryRootPin>
    load_local_goose_dialogue_responder_retry_root_pin(
        const std::string& trusted_absolute_path,
        const std::string& independently_enrolled_root_fingerprint);

    LocalGooseDialogueResponderRetryRootPin(
        const std::array<unsigned char, 32>& public_key,
        std::string fingerprint)
        : public_key_(public_key), fingerprint_(std::move(fingerprint))
    {
    }

    std::array<unsigned char, 32> public_key_;
    std::string fingerprint_;
};

// Read only. Does not create, enroll, update or rotate keys. Rejects
// noncanonical paths, symlink components, unsafe ancestors, unowned or
// non-0700 immediate parent, non-0400/non-regular/hardlinked file and
// any format/trust-fingerprint mismatch. A caller MUST NOT derive the
// independent fingerprint from the file, the proposal, or Drive.
//
// This is not an independent secure hardware anchor: same-UID tampering,
// an untrusted caller choosing its own path/fingerprint, and filesystem
// rollback remain outside this source-only verifier's trust boundary.
[[nodiscard]] std::optional<LocalGooseDialogueResponderRetryRootPin>
load_local_goose_dialogue_responder_retry_root_pin(
    const std::string& trusted_absolute_path,
    const std::string& independently_enrolled_root_fingerprint);

} // namespace gaudere_agent

#endif
