#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/types.h"

namespace kerndb {
class InMemoryCatalog;
struct InMemoryTable;
}

namespace kerndb::storage {

class TableStorage {
public:
    virtual ~TableStorage() = default;

    virtual Result<void> CreateTable(const InMemoryTable& table) = 0;
    virtual Result<RecordId> Insert(const InMemoryTable& table, const Tuple& tuple) = 0;
    virtual Result<std::vector<Tuple>> Scan(const InMemoryTable& table) const = 0;
};

class PersistentStorage;

[[nodiscard]] Result<std::unique_ptr<PersistentStorage>> OpenPersistentStorage(
    const std::filesystem::path& database_directory,
    InMemoryCatalog& catalog);

}  // namespace kerndb::storage
