#include <filesystem>
#include <fstream>

#include "kerndb/result.h"
#include "kerndb/storage/database_metadata.h"
#include "test_framework.h"
#include "temp_directory.h"

KERNDB_TEST(DatabaseMetadataCreatesStableValidatedDatabaseIdentity) {
    kerndb::test::TemporaryDirectory directory;
    const auto created = kerndb::storage::OpenOrCreateDatabaseMetadata(directory.path());
    KERNDB_EXPECT(created.ok());
    KERNDB_EXPECT(created.value().database_id != 0U);
    KERNDB_EXPECT(std::filesystem::exists(directory.path() / "database.meta"));

    const auto reopened = kerndb::storage::OpenOrCreateDatabaseMetadata(directory.path());
    KERNDB_EXPECT(reopened.ok());
    KERNDB_EXPECT_EQ(created.value().database_id, reopened.value().database_id);
}

KERNDB_TEST(DatabaseMetadataRejectsCorruptMagic) {
    kerndb::test::TemporaryDirectory directory;
    KERNDB_EXPECT(kerndb::storage::OpenOrCreateDatabaseMetadata(directory.path()).ok());

    const std::filesystem::path metadata_path = directory.path() / "database.meta";
    std::fstream file(metadata_path, std::ios::binary | std::ios::in | std::ios::out);
    KERNDB_EXPECT(file.is_open());
    file.seekp(0);
    file.put('X');
    file.flush();
    KERNDB_EXPECT(file.good());

    const auto reopened = kerndb::storage::OpenOrCreateDatabaseMetadata(directory.path());
    KERNDB_EXPECT(!reopened.ok());
    KERNDB_EXPECT_EQ(kerndb::ErrorCode::kCorruption, reopened.status().code());
}
