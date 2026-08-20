#include "OpenAIActivation.hpp"

#include <gaudere/budget/Store.hpp>
#include <gaudere/scheduling/wake/ActionStore.hpp>
#include <gaudere/scheduling/wake/Runtime.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

using namespace gaudere::scheduling::wake;
using namespace gaudere_agent;
using namespace std::chrono_literals;

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

class MemoryActionStore final : public ActionStore {
public:
    std::optional<Action> find(const std::string& id) const override
    {
        const auto found = actions.find(id);
        return found == actions.end() ? std::nullopt
                                      : std::optional<Action>{found->second};
    }

    std::optional<Action> find_by_idempotency_key(
        const std::string& key) const override
    {
        for (const auto& entry : actions) {
            if (entry.second.idempotency_key == key) {
                return entry.second;
            }
        }
        return std::nullopt;
    }

    std::vector<Action> running_with_expired_lease(TimePoint now) const override
    {
        std::vector<Action> result;
        for (const auto& entry : actions) {
            const auto& action = entry.second;
            if (action.status == ActionStatus::running && action.lease
                && action.lease->expires_at <= now) {
                result.push_back(action);
            }
        }
        return result;
    }

    bool has_running() const override
    {
        for (const auto& entry : actions) {
            if (entry.second.status == ActionStatus::running) {
                return true;
            }
        }
        return false;
    }

    void save(const Action& action) override { actions[action.id] = action; }

    std::map<std::string, Action> actions;
};

class MemoryBudgetStore final : public gaudere::budget::Store {
public:
    gaudere::budget::ConsumeResult consume(
        const std::string&,
        const std::string&,
        gaudere::budget::TimePoint,
        const gaudere::budget::Policy&) override
    {
        ++consumes;
        return gaudere::budget::ConsumeResult::accepted;
    }

    gaudere::budget::Snapshot snapshot(
        const std::string&,
        gaudere::budget::TimePoint,
        const gaudere::budget::Policy&) override
    {
        ++snapshots;
        return gaudere::budget::Snapshot{};
    }

    int consumes = 0;
    int snapshots = 0;
};

struct TemporaryDirectory {
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-openai-activation-test-" + std::to_string(
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

void write_secret(const std::filesystem::path& path, const std::string& value)
{
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create activation test secret");
        }
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    if (::chmod(path.c_str(), 0400) != 0) {
        throw std::runtime_error("cannot chmod activation test secret");
    }
}

void test_valid_activation_is_network_free()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "operator-key", "synthetic-key");
    MemoryActionStore store;
    MemoryBudgetStore budget_store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    OpenAIActivation activation(runtime, store, budget_store,
                                "gpt-test", "operator-key",
                                directory.path.string());
    expect(activation.model() == "gpt-test", "activation exposes configured model");
    expect(activation.secret_name() == "operator-key",
           "activation exposes only configured secret name");
    expect(store.actions.empty(),
           "constructing activation creates no durable external Action");
    expect(budget_store.consumes == 0 && budget_store.snapshots == 0,
           "constructing activation neither consumes nor inspects provider budget");

    const auto policy = activation.budget_policy();
    expect(policy.max_total == 12, "bootstrap budget allows twelve lifetime calls");
    expect(policy.max_in_window == 4, "bootstrap budget allows four calls per window");
    expect(policy.window == 24h, "bootstrap budget uses a rolling 24-hour window");
    expect(policy.min_interval == 15min,
           "bootstrap budget requires fifteen minutes between calls");
}

void test_missing_secret_fails_preflight()
{
    TemporaryDirectory directory;
    MemoryActionStore store;
    MemoryBudgetStore budget_store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    expect_throw<std::runtime_error>(
        [&] {
            OpenAIActivation activation(runtime, store, budget_store,
                                        "gpt-test", "missing",
                                        directory.path.string());
        },
        "missing OpenAI secret rejects activation");
    expect(store.actions.empty(), "failed preflight creates no Action");
    expect(budget_store.consumes == 0 && budget_store.snapshots == 0,
           "failed preflight does not touch provider call budget");
}

void test_newline_secret_fails_preflight()
{
    TemporaryDirectory directory;
    write_secret(directory.path / "newline-key", "synthetic-key\n");
    MemoryActionStore store;
    MemoryBudgetStore budget_store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    expect_throw<std::runtime_error>(
        [&] {
            OpenAIActivation activation(runtime, store, budget_store,
                                        "gpt-test", "newline-key",
                                        directory.path.string());
        },
        "newline-containing OpenAI secret rejects activation");
    expect(store.actions.empty(), "invalid credential creates no Action");
    expect(budget_store.consumes == 0 && budget_store.snapshots == 0,
           "invalid credential does not touch provider call budget");
}

void test_invalid_secret_name_fails_preflight()
{
    TemporaryDirectory directory;
    MemoryActionStore store;
    MemoryBudgetStore budget_store;
    Runtime runtime(store, [] { return TimePoint{}; });
    runtime.recover();

    expect_throw<std::invalid_argument>(
        [&] {
            OpenAIActivation activation(runtime, store, budget_store,
                                        "gpt-test", "../key",
                                        directory.path.string());
        },
        "invalid secret name cannot escape configured secret directory");
    expect(budget_store.consumes == 0 && budget_store.snapshots == 0,
           "invalid secret name does not touch provider call budget");
}

} // namespace

int main()
{
    test_valid_activation_is_network_free();
    test_missing_secret_fails_preflight();
    test_newline_secret_fails_preflight();
    test_invalid_secret_name_fails_preflight();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All OpenAI activation tests passed\n";
    return 0;
}
