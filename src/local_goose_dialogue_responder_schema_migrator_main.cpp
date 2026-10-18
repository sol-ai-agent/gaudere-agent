#include "LocalGooseDialogueResponderAuthorityStore.hpp"
#include "LocalGooseDialogueResponderStore.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

enum class Migration {
    v1_to_v2,
    v2_to_v3
};

struct Options {
    std::string responder_sidecar;
    std::optional<Migration> migration;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " migrate --responder-sidecar PATH --authorize-v1-to-v2\n"
        << "       " << program
        << " migrate --responder-sidecar PATH --authorize-v2-to-v3\n";
}

Options parse_options(int argc, char* argv[])
{
    if (argc >= 2 && std::string{argv[1]} == "--help") {
        usage(argv[0]);
        std::exit(0);
    }
    if (argc < 2 || std::string{argv[1]} != "migrate") {
        throw std::invalid_argument("schema migrator command must be migrate");
    }

    Options options;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--responder-sidecar") {
            if (index + 1 >= argc) {
                throw std::invalid_argument("missing value for --responder-sidecar");
            }
            options.responder_sidecar = argv[++index];
        } else if (argument == "--authorize-v1-to-v2") {
            if (options.migration) {
                throw std::invalid_argument(
                    "exactly one responder schema migration authorization is required");
            }
            options.migration = Migration::v1_to_v2;
        } else if (argument == "--authorize-v2-to-v3") {
            if (options.migration) {
                throw std::invalid_argument(
                    "exactly one responder schema migration authorization is required");
            }
            options.migration = Migration::v2_to_v3;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown schema migrator argument: " + argument);
        }
    }

    if (!options.migration) {
        throw std::invalid_argument(
            "explicit responder schema migration authorization is required");
    }
    if (options.responder_sidecar.empty()
        || options.responder_sidecar.front() != '/') {
        throw std::invalid_argument("responder sidecar path must be absolute");
    }
    const auto status = std::filesystem::symlink_status(options.responder_sidecar);
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument(
            "responder sidecar must be an existing regular non-symlink file");
    }
    return options;
}

std::int64_t scalar_int64(const std::string& path, const char* sql)
{
    sqlite3* database = nullptr;
    const int open_result = sqlite3_open_v2(
        path.c_str(), &database, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (open_result != SQLITE_OK) {
        const std::string detail = database ? sqlite3_errmsg(database)
                                            : "cannot open responder sidecar";
        if (database) sqlite3_close(database);
        throw std::runtime_error(detail);
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        const std::string detail = sqlite3_errmsg(database);
        sqlite3_close(database);
        throw std::runtime_error(detail);
    }
    const int step = sqlite3_step(statement);
    if (step != SQLITE_ROW) {
        const std::string detail = sqlite3_errmsg(database);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        throw std::runtime_error(detail);
    }
    const auto value = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

int user_version(const std::string& path)
{
    return static_cast<int>(scalar_int64(path, "PRAGMA user_version;"));
}

void migrate_v1_to_v2(const std::string& path)
{
    const int before = user_version(path);
    if (before != 1) {
        throw std::runtime_error(
            "responder sidecar must be exactly schema v1 before v1-to-v2 migration");
    }

    {
        gaudere_agent::LocalGooseDialogueResponderStore store(
            path,
            gaudere_agent::LocalGooseDialogueResponderStoreOpenMode::
                allow_schema_upgrade);
    }

    const int after = user_version(path);
    if (after != 2) {
        throw std::runtime_error(
            "responder sidecar did not reach exactly schema v2");
    }

    std::cout << "RESPONDER_SCHEMA_BEFORE=" << before << '\n'
              << "RESPONDER_SCHEMA_AFTER=" << after << '\n'
              << "RESPONDER_SCHEMA_MIGRATION=v1-to-v2\n";
}

void migrate_v2_to_v3(const std::string& path)
{
    const int before = user_version(path);
    if (before != 2) {
        throw std::runtime_error(
            "responder sidecar must be exactly schema v2 before v2-to-v3 migration");
    }

    {
        gaudere_agent::LocalGooseDialogueResponderAuthorityStore authority(
            path,
            gaudere_agent::LocalGooseDialogueResponderAuthorityStoreOpenMode::
                allow_schema_upgrade);
        if (authority.schema_version()
            != gaudere_agent::local_goose_dialogue_responder_authority_sidecar_schema) {
            throw std::runtime_error(
                "responder authority store did not report schema v3");
        }
    }

    const int after = user_version(path);
    if (after != 3) {
        throw std::runtime_error(
            "responder sidecar did not reach exactly schema v3");
    }
    const auto grants = scalar_int64(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_grant;");
    const auto decisions = scalar_int64(path,
        "SELECT COUNT(*) FROM local_goose_dialogue_responder_decision;");
    if (grants != 0 || decisions != 0) {
        throw std::runtime_error(
            "v2-to-v3 migration synthesized responder authority records");
    }

    std::cout << "RESPONDER_SCHEMA_BEFORE=" << before << '\n'
              << "RESPONDER_SCHEMA_AFTER=" << after << '\n'
              << "RESPONDER_SCHEMA_MIGRATION=v2-to-v3\n"
              << "RESPONDER_AUTHORITY_GRANTS=" << grants << '\n'
              << "RESPONDER_AUTHORITY_DECISIONS=" << decisions << '\n';
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        switch (*options.migration) {
        case Migration::v1_to_v2:
            migrate_v1_to_v2(options.responder_sidecar);
            break;
        case Migration::v2_to_v3:
            migrate_v2_to_v3(options.responder_sidecar);
            break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-dialogue-responder-schema-migrator: "
                  << error.what() << '\n';
        return 1;
    }
}
