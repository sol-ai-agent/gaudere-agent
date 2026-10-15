#include "LocalGooseDialogueResponderStore.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Options {
    std::string responder_sidecar;
    bool authorized = false;
};

void usage(const char* program)
{
    std::cout
        << "Usage: " << program
        << " migrate --responder-sidecar PATH --authorize-v1-to-v2\n";
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
            options.authorized = true;
        } else if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown schema migrator argument: " + argument);
        }
    }

    if (!options.authorized) {
        throw std::invalid_argument("explicit v1-to-v2 schema authorization is required");
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

int user_version(const std::string& path)
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
    if (sqlite3_prepare_v2(database, "PRAGMA user_version;", -1, &statement,
                           nullptr) != SQLITE_OK) {
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
    const int value = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return value;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        const auto options = parse_options(argc, argv);
        const int before = user_version(options.responder_sidecar);
        if (before != 1) {
            throw std::runtime_error(
                "responder sidecar must be exactly schema v1 before migration");
        }

        {
            gaudere_agent::LocalGooseDialogueResponderStore store(
                options.responder_sidecar,
                gaudere_agent::LocalGooseDialogueResponderStoreOpenMode::
                    allow_schema_upgrade);
        }

        const int after = user_version(options.responder_sidecar);
        if (after != 2) {
            throw std::runtime_error(
                "responder sidecar did not reach exactly schema v2");
        }

        std::cout << "RESPONDER_SCHEMA_BEFORE=" << before << '\n'
                  << "RESPONDER_SCHEMA_AFTER=" << after << '\n'
                  << "RESPONDER_SCHEMA_MIGRATION=v1-to-v2\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "gaudere-local-goose-dialogue-responder-schema-migrator: "
                  << error.what() << '\n';
        return 1;
    }
}
