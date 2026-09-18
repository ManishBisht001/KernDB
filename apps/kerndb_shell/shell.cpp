#include "shell.h"

#include <filesystem>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kerndb/database.h"
#include "kerndb/types.h"

namespace kerndb::shell {
namespace {

void PrintUsage(std::ostream& output) {
    output << "Usage: kerndb_shell [--database DIRECTORY] [--execute \"SQL\"]...\n"
           << "       kerndb_shell --help | --version\n"
           << "With no arguments, starts a one-line in-memory SQL REPL.\n";
}

void PrintResult(const QueryResult& result, std::ostream& output) {
    switch (result.kind) {
        case QueryResultKind::kCreateTable:
            output << "CREATE TABLE\n";
            return;
        case QueryResultKind::kInsert:
            output << "INSERT " << result.rows_affected << "\n";
            return;
        case QueryResultKind::kSelect:
            break;
    }

    for (std::size_t index = 0U; index < result.columns.size(); ++index) {
        if (index != 0U) {
            output << " | ";
        }
        output << result.columns[index].name;
    }
    output << "\n";
    for (const Tuple& row : result.rows) {
        for (std::size_t index = 0U; index < row.size(); ++index) {
            if (index != 0U) {
                output << " | ";
            }
            output << ToString(row[index]);
        }
        output << "\n";
    }
}

[[nodiscard]] bool ExecuteSql(
    Database& database,
    std::string_view sql,
    std::ostream& output,
    std::ostream& error) {
    const auto result = database.Execute(sql);
    if (!result.ok()) {
        error << "error: " << result.status().ToString() << "\n";
        return false;
    }
    PrintResult(result.value(), output);
    return true;
}

}  // namespace

int Run(
    int argc,
    const char* const argv[],
    std::istream& input,
    std::ostream& output,
    std::ostream& error) {
    std::optional<std::filesystem::path> database_directory;
    std::vector<std::string_view> statements;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") {
            PrintUsage(output);
            return 0;
        }
        if (argument == "--version") {
            const EngineIdentity identity = GetEngineIdentity();
            output << identity.name << " " << identity.version << "\n";
            return 0;
        }
        if (argument == "--database") {
            if (index + 1 >= argc || database_directory.has_value()) {
                error << "--database requires one directory and may only be specified once\n";
                return 2;
            }
            ++index;
            database_directory = std::filesystem::path(argv[index]);
            continue;
        }
        if (argument == "--execute") {
            if (index + 1 >= argc) {
                error << "Missing SQL after --execute\n";
                return 2;
            }
            ++index;
            statements.emplace_back(argv[index]);
            continue;
        }

        error << "Unknown option: " << argument << "\n";
        PrintUsage(error);
        return 2;
    }

    Database database;
    if (database_directory.has_value()) {
        auto opened = Database::Open(database_directory.value());
        if (!opened.ok()) {
            error << "error: " << opened.status().ToString() << "\n";
            return 1;
        }
        database = std::move(opened).value();
    }

    for (const std::string_view statement : statements) {
        if (!ExecuteSql(database, statement, output, error)) {
            return 1;
        }
    }
    if (!statements.empty()) {
        return 0;
    }

    output << (database_directory.has_value()
                   ? "KernDB Phase 2 persistent REPL. Type .quit to exit.\n"
                   : "KernDB Phase 1 in-memory REPL. Type .quit to exit.\n");
    std::string line;
    while (true) {
        output << "kerndb> ";
        if (!std::getline(input, line) || line == ".quit") {
            output << "\n";
            return 0;
        }
        if (!line.empty()) {
            static_cast<void>(ExecuteSql(database, line, output, error));
        }
    }
}

}  // namespace kerndb::shell
