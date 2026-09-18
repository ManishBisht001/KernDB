#include <array>
#include <istream>
#include <sstream>
#include <string>
#include <utility>

#include "shell.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(ShellHelpAndVersionAreAvailableWithoutDatabaseAccess) {
    {
        const std::array<const char*, 2U> arguments{"kerndb_shell", "--help"};
        std::istringstream input;
        std::ostringstream output;
        std::ostringstream error;
        const int exit_code = kerndb::shell::Run(
            static_cast<int>(arguments.size()),
            arguments.data(),
            input,
            output,
            error);

        KERNDB_EXPECT_EQ(0, exit_code);
        KERNDB_EXPECT(output.str().find("Usage: kerndb_shell") != std::string::npos);
        KERNDB_EXPECT(error.str().empty());
    }

    {
        const std::array<const char*, 2U> arguments{"kerndb_shell", "--version"};
        std::istringstream input;
        std::ostringstream output;
        std::ostringstream error;
        const int exit_code = kerndb::shell::Run(
            static_cast<int>(arguments.size()),
            arguments.data(),
            input,
            output,
            error);

        KERNDB_EXPECT_EQ(0, exit_code);
        KERNDB_EXPECT_EQ(std::string("KernDB 0.0.0\n"), output.str());
        KERNDB_EXPECT(error.str().empty());
    }
}

KERNDB_TEST(ShellExecutesMultipleStatementsAgainstOneInMemoryDatabase) {
    const std::array<const char*, 7U> arguments{
        "kerndb_shell",
        "--execute",
        "CREATE TABLE people (id INT, name TEXT);",
        "--execute",
        "INSERT INTO people VALUES (1, 'Ada');",
        "--execute",
        "SELECT id, name FROM people WHERE id = 1;",
    };
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;

    const int exit_code = kerndb::shell::Run(
        static_cast<int>(arguments.size()),
        arguments.data(),
        input,
        output,
        error);

    KERNDB_EXPECT_EQ(0, exit_code);
    KERNDB_EXPECT(output.str().find("CREATE TABLE") != std::string::npos);
    KERNDB_EXPECT(output.str().find("INSERT 1") != std::string::npos);
    KERNDB_EXPECT(output.str().find("1 | Ada") != std::string::npos);
    KERNDB_EXPECT(error.str().empty());
}

KERNDB_TEST(ShellUsesPersistentDatabaseDirectoryAcrossRuns) {
    kerndb::test::TemporaryDirectory directory;
    const std::string database_path = (directory.path() / "database").string();
    const std::array<const char*, 7U> write_arguments{
        "kerndb_shell",
        "--database",
        database_path.c_str(),
        "--execute",
        "CREATE TABLE people (id INT, name TEXT);",
        "--execute",
        "INSERT INTO people VALUES (1, 'Ada');",
    };
    std::istringstream write_input;
    std::ostringstream write_output;
    std::ostringstream write_error;
    KERNDB_EXPECT_EQ(
        0,
        kerndb::shell::Run(
            static_cast<int>(write_arguments.size()),
            write_arguments.data(),
            write_input,
            write_output,
            write_error));
    KERNDB_EXPECT(write_error.str().empty());

    const std::array<const char*, 5U> read_arguments{
        "kerndb_shell",
        "--database",
        database_path.c_str(),
        "--execute",
        "SELECT id, name FROM people WHERE id = 1;",
    };
    std::istringstream read_input;
    std::ostringstream read_output;
    std::ostringstream read_error;
    KERNDB_EXPECT_EQ(
        0,
        kerndb::shell::Run(
            static_cast<int>(read_arguments.size()),
            read_arguments.data(),
            read_input,
            read_output,
            read_error));
    KERNDB_EXPECT(read_error.str().empty());
    KERNDB_EXPECT(read_output.str().find("1 | Ada") != std::string::npos);
}
