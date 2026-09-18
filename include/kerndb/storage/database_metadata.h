#pragma once

#include <cstdint>
#include <filesystem>

#include "kerndb/result.h"

namespace kerndb::storage {

struct DatabaseMetadata {
    std::uint64_t database_id;
};

[[nodiscard]] Result<DatabaseMetadata> OpenOrCreateDatabaseMetadata(
    const std::filesystem::path& database_directory);

}  // namespace kerndb::storage
