#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/types.h"

namespace kerndb {
class InMemoryCatalog;
struct InMemoryTable;
struct InMemoryIndex;
}

namespace kerndb::storage {

struct IndexLookup {
    bool index_available{false};
    std::optional<Tuple> tuple;
};

class TableStorage {
public:
    virtual ~TableStorage() = default;

    virtual Result<void> CreateTable(const InMemoryTable& table) = 0;
    virtual Result<PageId> CreateIndex(const InMemoryIndex& index) = 0;
    virtual Result<RecordId> Insert(const InMemoryTable& table, const Tuple& tuple) = 0;
    virtual Result<std::vector<Tuple>> Scan(const InMemoryTable& table) const = 0;
    virtual Result<IndexLookup> LookupByIndex(
        const InMemoryTable& table,
        std::size_t column_index,
        std::int64_t key) const = 0;
};

class PersistentStorage;

[[nodiscard]] Result<std::unique_ptr<PersistentStorage>> OpenPersistentStorage(
    const std::filesystem::path& database_directory,
    InMemoryCatalog& catalog);

}  // namespace kerndb::storage
