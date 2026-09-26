#include "LocalGooseDialogueThreadStore.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/stat.h>

namespace {

using namespace gaudere_agent;

std::string task_id(const std::string& version, const char digit)
{
    return "cognition.local-goose-dialogue." + version + ":"
        + std::string(64, digit);
}

struct TemporarySidecar {
    TemporarySidecar()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-dialogue-thread-store-test-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch().count())
               + ".db");
    }

    ~TemporarySidecar()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-journal", ignored);
    }

    std::filesystem::path path;
};

} // namespace

int main()
{
    TemporarySidecar sidecar;
    LocalGooseDialogueThreadStore store(sidecar.path.string());

    struct stat status {};
    assert(::stat(sidecar.path.c_str(), &status) == 0);
    assert(S_ISREG(status.st_mode));
    assert((status.st_mode & 0777) == 0600);

    LocalGooseDialogueThreadHead initial;
    initial.alias = "main";
    initial.revision = 0;
    initial.root_task_id = task_id("v2", 'a');
    initial.head_task_id = task_id("v2", 'b');

    assert(valid_local_goose_dialogue_thread_head(initial));
    assert(!store.find("main"));

    const auto seeded = store.seed(initial);
    assert(seeded.result == LocalGooseDialogueThreadStoreResult::accepted);
    assert(seeded.head);
    assert(seeded.head->revision == 0);
    assert(seeded.head->head_task_id == initial.head_task_id);

    const auto duplicate_seed = store.seed(initial);
    assert(duplicate_seed.result
        == LocalGooseDialogueThreadStoreResult::duplicate);

    auto conflicting_seed = initial;
    conflicting_seed.head_task_id = task_id("v2", 'c');
    const auto seed_conflict = store.seed(conflicting_seed);
    assert(seed_conflict.result
        == LocalGooseDialogueThreadStoreResult::conflict);
    assert(seed_conflict.head);
    assert(seed_conflict.head->head_task_id == initial.head_task_id);

    auto advanced = initial;
    advanced.revision = 1;
    advanced.head_task_id = task_id("v3", 'd');
    assert(valid_local_goose_dialogue_thread_head_transition(
        initial, advanced));

    const auto replaced = store.replace(initial, advanced);
    assert(replaced.result
        == LocalGooseDialogueThreadStoreResult::accepted);
    assert(replaced.head);
    assert(replaced.head->revision == 1);
    assert(replaced.head->root_task_id == initial.root_task_id);
    assert(replaced.head->head_task_id == advanced.head_task_id);

    const auto duplicate_replace = store.replace(initial, advanced);
    assert(duplicate_replace.result
        == LocalGooseDialogueThreadStoreResult::duplicate);

    auto stale_replacement = initial;
    stale_replacement.revision = 1;
    stale_replacement.head_task_id = task_id("v3", 'e');
    const auto stale = store.replace(initial, stale_replacement);
    assert(stale.result == LocalGooseDialogueThreadStoreResult::conflict);
    assert(stale.head);
    assert(stale.head->revision == 1);
    assert(stale.head->head_task_id == advanced.head_task_id);

    auto next = advanced;
    next.revision = 2;
    next.head_task_id = task_id("v3", 'f');
    const auto second = store.replace(advanced, next);
    assert(second.result
        == LocalGooseDialogueThreadStoreResult::accepted);

    auto changed_root = next;
    changed_root.revision = 3;
    changed_root.root_task_id = task_id("v3", '1');
    changed_root.head_task_id = task_id("v3", '2');
    assert(!valid_local_goose_dialogue_thread_head_transition(
        next, changed_root));
    const auto invalid_root_change = store.replace(next, changed_root);
    assert(invalid_root_change.result
        == LocalGooseDialogueThreadStoreResult::invalid);

    auto bad_alias = initial;
    bad_alias.alias = "bad alias";
    assert(!valid_local_goose_dialogue_thread_head(bad_alias));
    assert(store.seed(bad_alias).result
        == LocalGooseDialogueThreadStoreResult::invalid);

    auto bad_id = initial;
    bad_id.root_task_id = "not-a-dialogue-task";
    assert(!valid_local_goose_dialogue_thread_head(bad_id));

    const auto history = store.history_from("main", 0, 16);
    assert(history.size() == 3);
    assert(history[0].revision == 0
        && history[0].head_task_id == initial.head_task_id);
    assert(history[1].revision == 1
        && history[1].head_task_id == advanced.head_task_id);
    assert(history[2].revision == 2
        && history[2].head_task_id == next.head_task_id);
    const auto tail = store.history_from("main", 2, 1);
    assert(tail.size() == 1 && tail.front().revision == 2);
    assert(store.history_from("main", 0, 0).empty());
    assert(store.history_from("bad alias", 0, 16).empty());

    const auto report = local_goose_dialogue_thread_head_report(next);
    assert(report.find("alias=\"main\"") != std::string::npos);
    assert(report.find("revision=2") != std::string::npos);
    assert(report.find(next.root_task_id) != std::string::npos);
    assert(report.find(next.head_task_id) != std::string::npos);

    std::cout << "local_goose_dialogue_thread_store_test: PASS\n";
    return 0;
}
