#include "LocalGooseCycleStore.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

std::string hex(const char c) { return std::string(64, c); }

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 5) {
        std::cerr << "usage: fixture STATE CYCLE MODEL GOVERNANCE\n";
        return 2;
    }

    const std::string state = argv[1];
    const std::string cycle = argv[2];
    const std::string model = argv[3];
    const std::string governance = argv[4];

    std::remove(state.c_str());
    std::remove(cycle.c_str());
    std::remove(model.c_str());
    std::remove(governance.c_str());

    std::ofstream(state).put('\n');
    std::ofstream(governance).put('\n');

    gaudere_agent::LocalGooseCycleCursor cursor;
    cursor.anchor_observation_task_id =
        "continuity.local-observation.v1:" + hex('a');
    cursor.anchor_observation_result_sha256 = hex('b');
    assert(gaudere_agent::valid_local_goose_cycle_cursor(cursor));

    gaudere_agent::LocalGooseCycleStore store(cycle);
    const auto seeded = store.seed(cursor);
    assert(seeded.result == gaudere_agent::LocalGooseCycleStoreResult::accepted);

    std::cout << "local Goose cycle runtime preflight fixture: ok\n";
    return 0;
}
