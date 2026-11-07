#include "LocalGooseDialogueCompletionFeed.hpp"
#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderRetryEligibility.hpp"
#include "LocalGooseDialogueResponderRetryStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"
#include "LocalGooseDialogueThreadStore.hpp"

#include <gaudere/persistence/sqlite/TaskStore.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace gaudere_agent;
namespace fs = std::filesystem;

constexpr const char* state_path = "/rehearsal/state.db";
constexpr const char* thread_path = "/rehearsal/local-goose-dialogue-thread.db";
constexpr const char* completion_path = "/rehearsal/local-goose-dialogue-completion.db";
constexpr const char* responder_path = "/rehearsal/local-goose-dialogue-responder.db";
constexpr const char* retry_path =
    "/rehearsal/local-goose-dialogue-responder-retry.db";

void require(const bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}

void require_frozen_copy()
{
    const auto root = fs::symlink_status("/rehearsal");
    require(fs::is_directory(root), "rehearsal directory missing or unsafe");
    for (const char* path : {state_path, thread_path, completion_path,
                             responder_path, retry_path}) {
        const auto file = fs::symlink_status(path);
        require(fs::is_regular_file(file),
                "rehearsal snapshot file missing or unsafe");
    }
}

std::int64_t inspection_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void inspect_generation_1()
{
    require_frozen_copy();

    // These historical stores are opened only on an isolated writable COPY.
    // Their constructors do not promise read-only filesystem behavior.
    gaudere::persistence::sqlite::TaskStore tasks{state_path};
    LocalGooseDialogueThreadStore thread{thread_path};
    LocalGooseDialogueCompletionFeedStore completion{completion_path};
    LocalGooseDialogueResponderStore responder{responder_path};
    LocalGooseDialogueResponderAuthorityStore authority{responder_path};

    // Unlike the legacy retry-store constructor, this mode MUST NOT create,
    // migrate or write the retry audit sidecar, even if it is absent.
    const LocalGooseDialogueResponderRetryStore retry{
        retry_path, LocalGooseDialogueResponderRetryStoreOpenMode::existing_read_only
    };
    require(retry.is_read_only(), "retry sidecar is not read-only");

    const LocalGooseDialogueResponderRetryEligibilityService eligibility{
        completion, thread, responder, authority, tasks, retry
    };
    const auto result = eligibility.inspect_generation_1(
        local_goose_dialogue_responder_first_retry_lineage_id,
        inspection_now_ms());

    if (result.code !=
        LocalGooseDialogueResponderRetryEligibilityCode::
            evidence_consistent_no_authority) {
        throw std::runtime_error(
            "retry evidence rejected (no production authority): " + result.detail);
    }

    std::cout
        << "RETRY_SIDECAR_READ_ONLY=yes\n"
        << "RETRY_GENERATION=1\n"
        << "LINEAGE_ID="
        << local_goose_dialogue_responder_first_retry_lineage_id << '\n'
        << "ELIGIBILITY=evidence_consistent_no_authority\n"
        << "RETRY_AUTHORITY=absent\n"
        << "GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_INSPECT=PASS\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string{argv[1]} == "--help") {
            std::cout << "Usage: retry-frozen-snapshot <inspect-generation-1>"
                         " (fixed /rehearsal paths only)\n";
            return 0;
        }
        require(argc == 2 && std::string{argv[1]} == "inspect-generation-1",
                "only inspect-generation-1 is supported");
        inspect_generation_1();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "GAUDERE_RESPONDER_RETRY_FROZEN_SNAPSHOT_INSPECT=ABORTED\n"
                  << e.what() << '\n';
        return 2;
    }
}
