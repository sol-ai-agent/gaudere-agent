#include "StateLock.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct TemporaryPath {
    TemporaryPath()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-agent-lock-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    }

    ~TemporaryPath()
    {
        std::error_code ignored;
        std::filesystem::remove(path.string() + ".lock", ignored);
    }

    std::filesystem::path path;
};

} // namespace

int main()
{
    TemporaryPath temporary;
    {
        gaudere_agent::StateLock first(temporary.path.string());
        bool rejected = false;
        try {
            gaudere_agent::StateLock second(temporary.path.string());
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected, "second owner is rejected while first lock is held");
    }

    bool reacquired = true;
    try {
        gaudere_agent::StateLock after_release(temporary.path.string());
    } catch (...) {
        reacquired = false;
    }
    expect(reacquired, "state ownership can be reacquired after release");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All state lock tests passed\n";
    return 0;
}
