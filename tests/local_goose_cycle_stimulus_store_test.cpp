#include "LocalGooseCycleStimulusStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using gaudere_agent::LocalGooseCycleStimulus;
using gaudere_agent::LocalGooseCycleStimulusStatus;
using gaudere_agent::LocalGooseCycleStimulusStore;
using gaudere_agent::LocalGooseCycleStimulusStoreResult;
using gaudere_agent::inspect_local_goose_cycle_stimulus_sidecar;
using gaudere_agent::make_explicit_local_recheck_stimulus;
using gaudere_agent::valid_local_goose_cycle_stimulus;
using gaudere_agent::valid_local_goose_cycle_stimulus_transition;

std::string temporary_path()
{
    const auto now = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return (std::filesystem::temp_directory_path()
        / ("gaudere-local-goose-stimulus-"
           + std::to_string(static_cast<long long>(::getpid()))
           + "-" + std::to_string(static_cast<long long>(now))
           + ".db")).string();
}

LocalGooseCycleStimulus consumed(
    const LocalGooseCycleStimulus& accepted,
    const std::int64_t terminal_at_ms)
{
    auto result = accepted;
    result.status = LocalGooseCycleStimulusStatus::consumed;
    result.terminal_at_ms = terminal_at_ms;
    result.resulting_cycle_revision =
        accepted.target_cycle_revision + 1;
    return result;
}

LocalGooseCycleStimulus superseded(
    const LocalGooseCycleStimulus& accepted,
    const std::int64_t terminal_at_ms)
{
    auto result = accepted;
    result.status = LocalGooseCycleStimulusStatus::superseded;
    result.terminal_at_ms = terminal_at_ms;
    result.terminal_reason = "target cursor changed before consumption";
    return result;
}

void remove_if_present(const std::string& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}

void test_identity_and_validation()
{
    const auto stimulus =
        make_explicit_local_recheck_stimulus("operator-recheck-001", 100, 3, 2);
    assert(valid_local_goose_cycle_stimulus(stimulus));
    assert(stimulus.id.rfind(
        gaudere_agent::local_goose_cycle_stimulus_id_prefix, 0) == 0);

    const auto duplicate =
        make_explicit_local_recheck_stimulus("operator-recheck-001", 999, 3, 2);
    assert(duplicate.id == stimulus.id);
    assert(duplicate.accepted_at_ms != stimulus.accepted_at_ms);

    bool rejected = false;
    try {
        static_cast<void>(make_explicit_local_recheck_stimulus(
            "free form text is forbidden", 100, 3, 2));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        static_cast<void>(make_explicit_local_recheck_stimulus(
            "too-early-generation", 100, 1, 1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

void test_append_duplicate_conflict_and_transition()
{
    const auto path = temporary_path();
    remove_if_present(path);

    {
        LocalGooseCycleStimulusStore store(path);
        struct stat status {};
        assert(::stat(path.c_str(), &status) == 0);
        assert(S_ISREG(status.st_mode));
        assert((status.st_mode & 0777) == 0600);

        const auto first =
            make_explicit_local_recheck_stimulus("recheck-001", 100, 3, 2);
        const auto write = store.append(first);
        assert(write.result == LocalGooseCycleStimulusStoreResult::accepted);
        assert(write.stimulus && write.stimulus->id == first.id);

        const auto duplicate = store.append(first);
        assert(duplicate.result
            == LocalGooseCycleStimulusStoreResult::duplicate);

        auto same_identity_different_acceptance = first;
        same_identity_different_acceptance.accepted_at_ms = 101;
        const auto source_conflict =
            store.append(same_identity_different_acceptance);
        assert(source_conflict.result
            == LocalGooseCycleStimulusStoreResult::conflict);

        const auto same_target =
            make_explicit_local_recheck_stimulus("recheck-002", 102, 3, 2);
        const auto target_conflict = store.append(same_target);
        assert(target_conflict.result
            == LocalGooseCycleStimulusStoreResult::conflict);

        const auto settled = consumed(first, 110);
        assert(valid_local_goose_cycle_stimulus_transition(first, settled));
        const auto replaced = store.replace(first, settled);
        assert(replaced.result
            == LocalGooseCycleStimulusStoreResult::accepted);
        assert(replaced.stimulus
            && replaced.stimulus->status
                == LocalGooseCycleStimulusStatus::consumed);
        assert(replaced.stimulus->resulting_cycle_revision
            == std::optional<std::uint64_t>{4});

        const auto duplicate_settle = store.replace(first, settled);
        assert(duplicate_settle.result
            == LocalGooseCycleStimulusStoreResult::duplicate);

        const auto found = store.find(first.id);
        assert(found && found->status
            == LocalGooseCycleStimulusStatus::consumed);
        assert(!store.find_accepted_for_target(3));

        const auto second =
            make_explicit_local_recheck_stimulus("recheck-003", 120, 4, 2);
        assert(store.append(second).result
            == LocalGooseCycleStimulusStoreResult::accepted);

        const auto stale = superseded(second, 130);
        assert(valid_local_goose_cycle_stimulus_transition(second, stale));
        assert(store.replace(second, stale).result
            == LocalGooseCycleStimulusStoreResult::accepted);
    }

    const auto inspection =
        inspect_local_goose_cycle_stimulus_sidecar(path);
    assert(inspection.eligible);
    assert(inspection.stimuli.size() == 2);
    assert(inspection.stimuli[0].source_id == "recheck-001");
    assert(inspection.stimuli[0].status
        == LocalGooseCycleStimulusStatus::consumed);
    assert(inspection.stimuli[1].source_id == "recheck-003");
    assert(inspection.stimuli[1].status
        == LocalGooseCycleStimulusStatus::superseded);

    {
        LocalGooseCycleStimulusStore reopened(path);
        const auto found = reopened.find_by_source("recheck-003");
        assert(found && found->status
            == LocalGooseCycleStimulusStatus::superseded);
    }

    remove_if_present(path);
}

void test_read_only_inspector_fails_closed_on_permissions()
{
    const auto path = temporary_path();
    remove_if_present(path);

    {
        LocalGooseCycleStimulusStore store(path);
        const auto stimulus =
            make_explicit_local_recheck_stimulus("recheck-mode", 200, 8, 4);
        assert(store.append(stimulus).result
            == LocalGooseCycleStimulusStoreResult::accepted);
    }

    assert(::chmod(path.c_str(), 0644) == 0);
    const auto inspection =
        inspect_local_goose_cycle_stimulus_sidecar(path);
    assert(!inspection.eligible);
    assert(!inspection.detail.empty());

    assert(::chmod(path.c_str(), 0600) == 0);
    remove_if_present(path);
}

} // namespace

int main()
{
    test_identity_and_validation();
    test_append_duplicate_conflict_and_transition();
    test_read_only_inspector_fails_closed_on_permissions();
    std::cout << "local_goose_cycle_stimulus_store_test: PASS\n";
    return 0;
}
