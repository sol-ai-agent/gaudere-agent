#include "ResumeContextSnapshot.hpp"

#include <gaudere/persistence/sqlite/ActionStore.hpp>
#include <gaudere/persistence/sqlite/BudgetStore.hpp>
#include <gaudere/persistence/sqlite/TaskStore.hpp>
#include <gaudere/persistence/sqlite/WakeIntentStore.hpp>
#include <gaudere/work/Runtime.hpp>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using Json = nlohmann::json;
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

struct TemporaryDatabase {
    explicit TemporaryDatabase(std::string label)
    {
        path = std::filesystem::temp_directory_path()
            / ("gaudere-agent-" + std::move(label) + "-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + ".db");
    }
    ~TemporaryDatabase()
    {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(path.string() + "-wal", ignored);
        std::filesystem::remove(path.string() + "-shm", ignored);
    }
    std::filesystem::path path;
};

Json provenance(std::string kind = "runtime-snapshot",
                std::string ref = "test",
                std::string digest = std::string(64, '0'))
{
    return Json{{"kind", std::move(kind)},
                {"ref", std::move(ref)},
                {"sha256", std::move(digest)}};
}

std::string request(std::string content = "current",
                    Json sources = Json::array({provenance()}),
                    std::string content_type = "text/plain; charset=utf-8")
{
    return Json{{"schema", resume_context_snapshot_schema},
                {"content_type", std::move(content_type)},
                {"content", std::move(content)},
                {"provenance", std::move(sources)}}.dump();
}

std::int64_t count_rows(const std::filesystem::path& path,
                        const char* table)
{
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("could not open sqlite test database");
    }
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(table);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        sqlite3_close(database);
        throw std::runtime_error("could not prepare sqlite count");
    }
    const auto result = sqlite3_step(statement);
    const auto count = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (count < 0) throw std::runtime_error("could not read sqlite count");
    return count;
}

struct Fixture {
    explicit Fixture(const std::filesystem::path& path)
        : tasks(path.string()), actions(path.string()), budgets(path.string()),
          wakes(path.string()), now(gaudere::work::TimePoint{1000ms}),
          runtime(tasks, [this] { return now; }),
          recorder(tasks, runtime, [this] { return now; })
    {
        runtime.recover();
    }

    gaudere::persistence::sqlite::TaskStore tasks;
    gaudere::persistence::sqlite::ActionStore actions;
    gaudere::persistence::sqlite::BudgetStore budgets;
    gaudere::persistence::sqlite::WakeIntentStore wakes;
    gaudere::work::TimePoint now;
    gaudere::work::Runtime runtime;
    ResumeContextSnapshotRecorder recorder;
};

void expect_invalid(Fixture& fixture, const std::string& raw,
                    const std::string& label)
{
    const auto result = fixture.recorder.record(raw);
    expect(result.result == ResumeContextSnapshotRecordResult::invalid,
           label + ": rejected as invalid");
}

void test_canonical_hash_and_idempotence()
{
    TemporaryDatabase database("resume-context-basic");
    Fixture fixture(database.path);
    const auto first = fixture.recorder.record(request());
    expect(first.result == ResumeContextSnapshotRecordResult::accepted,
           "first snapshot accepted");
    expect(first.task.has_value(), "first snapshot returned durable Task");
    const std::string expected_id =
        "continuity.resume-context-snapshot.v1:"
        "a6fe840a4025d8f0208e5676df5d0bb72401a8f6aca14e2b455a11cd038b03d7";
    if (first.task) {
        expect(first.task->id == expected_id, "SHA-256 canonical vector matches");
        expect(first.task->idempotency_key == expected_id,
               "snapshot id and idempotency key match");
        expect(first.task->status == gaudere::work::TaskStatus::succeeded,
               "snapshot Task is terminal succeeded");
        expect(first.task->result && first.task->result->output == first.task->input,
               "snapshot result is exact canonical capsule");
        expect(first.task->input.find("\"captured_at_ms\":1000") != std::string::npos,
               "recorder clock assigned captured_at_ms");
    }

    const auto duplicate = fixture.recorder.record(request());
    expect(duplicate.result == ResumeContextSnapshotRecordResult::duplicate,
           "same capsule at same clock is idempotent");
    expect(duplicate.task && duplicate.task->id == expected_id,
           "same capsule preserves content-addressed identity");

    fixture.now += 1ms;
    const auto later = fixture.recorder.record(request());
    expect(later.result == ResumeContextSnapshotRecordResult::accepted,
           "same source data at a later capture time is a new snapshot");
    expect(later.task && later.task->id != expected_id,
           "later capture has a distinct content hash");

    expect(count_rows(database.path, "actions") == 0,
           "snapshot recording creates no Action");
    expect(count_rows(database.path, "budget_consumptions") == 0,
           "snapshot recording consumes no provider budget");
    expect(count_rows(database.path, "wake_intents") == 0,
           "snapshot recording creates no WakeIntent");
}

void test_validation_bounds()
{
    TemporaryDatabase database("resume-context-validation");
    Fixture fixture(database.path);

    expect_invalid(fixture, "not-json", "malformed JSON");
    expect_invalid(fixture,
        "{\"schema\":\"gaudere.resume-current-context.v1\","
        "\"schema\":\"gaudere.resume-current-context.v1\","
        "\"content_type\":\"text/plain; charset=utf-8\","
        "\"content\":\"x\",\"provenance\":[{\"kind\":\"runtime-snapshot\","
        "\"ref\":\"x\",\"sha256\":\"" + std::string(64, '0') + "\"}]}",
        "duplicate root key");

    auto unknown = Json::parse(request());
    unknown["captured_at_ms"] = 1;
    expect_invalid(fixture, unknown.dump(), "caller-selected capture time");

    auto nested_unknown = Json::parse(request());
    nested_unknown["provenance"][0]["extra"] = true;
    expect_invalid(fixture, nested_unknown.dump(), "unknown provenance key");

    expect_invalid(fixture, request("", Json::array({provenance()})),
                   "empty content");
    expect_invalid(fixture, request(std::string(16385, 'x')),
                   "content over 16 KiB");
    expect(fixture.recorder.record(request(std::string(16384, 'x'))).result
               == ResumeContextSnapshotRecordResult::accepted,
           "content at 16 KiB accepted");

    expect_invalid(fixture, request("x", Json::array()), "empty provenance");
    Json nine = Json::array();
    for (int index = 0; index < 9; ++index) nine.push_back(provenance());
    expect_invalid(fixture, request("x", nine), "more than eight provenance entries");

    expect_invalid(fixture,
                   request("x", Json::array({provenance("unknown")})),
                   "unknown provenance kind");
    expect_invalid(fixture,
                   request("x", Json::array({provenance(
                       "runtime-snapshot", "line\nbreak")})),
                   "control character in provenance ref");
    expect_invalid(fixture,
                   request("x", Json::array({provenance(
                       "runtime-snapshot", "x", std::string(64, 'A'))})),
                   "uppercase provenance digest");
    expect_invalid(fixture, request("x", Json::array({provenance()}),
                                   "application/json"),
                   "unsupported content type");

    std::string invalid_utf8 = "{\"schema\":\"gaudere.resume-current-context.v1\","
        "\"content_type\":\"text/plain; charset=utf-8\",\"content\":\"";
    invalid_utf8.push_back(static_cast<char>(0xc0));
    invalid_utf8 += "\",\"provenance\":[{\"kind\":\"runtime-snapshot\","
        "\"ref\":\"x\",\"sha256\":\"" + std::string(64, '0') + "\"}]}";
    expect_invalid(fixture, invalid_utf8, "invalid UTF-8");
}

void test_pending_crash_recovery()
{
    TemporaryDatabase database("resume-context-recovery");
    gaudere::persistence::sqlite::TaskStore tasks(database.path.string());
    auto now = gaudere::work::TimePoint{1000ms};
    gaudere::work::Runtime runtime(tasks, [&] { return now; });
    runtime.recover();

    const std::string canonical =
        "{\"captured_at_ms\":1000,\"content\":\"current\","
        "\"content_type\":\"text/plain; charset=utf-8\","
        "\"provenance\":[{\"kind\":\"runtime-snapshot\",\"ref\":\"test\","
        "\"sha256\":\"" + std::string(64, '0') + "\"}],"
        "\"schema\":\"gaudere.resume-current-context.v1\"}";
    gaudere::work::Task task;
    task.id = "continuity.resume-context-snapshot.v1:"
        "a6fe840a4025d8f0208e5676df5d0bb72401a8f6aca14e2b455a11cd038b03d7";
    task.idempotency_key = task.id;
    task.kind = resume_context_snapshot_task_kind;
    task.input_content_type = resume_context_snapshot_content_type;
    task.input = canonical;
    task.limits.max_input_bytes = 24 * 1024;
    task.limits.max_output_bytes = 24 * 1024;
    task.limits.max_runtime = 2s;
    task.limits.max_attempts = 2;
    expect(runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "crash fixture persisted pending snapshot Task");

    // This models a crash immediately after submit. A new recorder sees and
    // completes the old pending Task before assigning a new capture time.
    ResumeContextSnapshotRecorder recorder(tasks, runtime, [&] { return now; });
    const auto recovered = recorder.record(request());
    expect(recovered.result == ResumeContextSnapshotRecordResult::duplicate,
           "pending same-request snapshot is recovered as duplicate");
    expect(recovered.task
               && recovered.task->status == gaudere::work::TaskStatus::succeeded,
           "recovered pending snapshot becomes succeeded");
    expect(recovered.task && recovered.task->attempts_started == 1,
           "recovered snapshot executes exactly once");

    // Crash while running: after lease expiry, Runtime::recover makes it pending
    // again because max_attempts=2, then the recorder finishes the same Task.
    TemporaryDatabase running_db("resume-context-running-recovery");
    gaudere::persistence::sqlite::TaskStore running_tasks(running_db.path.string());
    auto running_now = gaudere::work::TimePoint{1000ms};
    gaudere::work::Runtime first_runtime(running_tasks, [&] { return running_now; });
    first_runtime.recover();
    expect(first_runtime.submit(task) == gaudere::work::SubmitResult::accepted,
           "running crash fixture submitted");
    expect(first_runtime.start(task.id, "crashed-recorder"),
           "running crash fixture started");
    running_now += 3s;
    gaudere::work::Runtime reopened(running_tasks, [&] { return running_now; });
    reopened.recover();
    // Retry uses the same logical request; the recovered pending Task retains its
    // original captured_at=1000 rather than silently becoming a new snapshot.
    ResumeContextSnapshotRecorder reopened_recorder(
        running_tasks, reopened, [&] { return running_now; });
    const auto after_reopen = reopened_recorder.record(request());
    expect(after_reopen.result == ResumeContextSnapshotRecordResult::duplicate,
           "expired running snapshot is recovered as same logical request");
    expect(after_reopen.task && after_reopen.task->id == task.id,
           "running crash recovery preserves original snapshot identity");
    expect(after_reopen.task && after_reopen.task->attempts_started == 2,
           "running crash recovery consumes only the allowed second local attempt");
}

void test_conflict_fails_closed()
{
    TemporaryDatabase database("resume-context-conflict");
    Fixture fixture(database.path);
    gaudere::work::Task conflicting;
    conflicting.id = "continuity.resume-context-snapshot.v1:"
        "a6fe840a4025d8f0208e5676df5d0bb72401a8f6aca14e2b455a11cd038b03d7";
    conflicting.idempotency_key = conflicting.id;
    conflicting.kind = resume_context_snapshot_task_kind;
    conflicting.input_content_type = resume_context_snapshot_content_type;
    conflicting.input = "{}";
    conflicting.limits.max_input_bytes = 24 * 1024;
    conflicting.limits.max_output_bytes = 24 * 1024;
    conflicting.limits.max_runtime = 2s;
    conflicting.limits.max_attempts = 2;
    fixture.tasks.save(conflicting);

    const auto result = fixture.recorder.record(request());
    expect(result.result == ResumeContextSnapshotRecordResult::conflict,
           "corrupt/conflicting durable Task fails closed");
}

} // namespace

int main()
{
    try {
        test_canonical_hash_and_idempotence();
        test_validation_bounds();
        test_pending_crash_recovery();
        test_conflict_fails_closed();
    } catch (const std::exception& error) {
        std::cerr << "UNCAUGHT: " << error.what() << '\n';
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " resume context snapshot assertion(s) failed\n";
        return 1;
    }
    std::cout << "resume context snapshot provider-free tests: PASS\n";
    return 0;
}
