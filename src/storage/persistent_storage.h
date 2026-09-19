#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

#include "catalog_store.h"
#include "kerndb/storage/table_storage.h"

namespace kerndb::storage {

class PersistentStorage final : public TableStorage {
public:
    [[nodiscard]] static Result<std::unique_ptr<PersistentStorage>> Open(
        const std::filesystem::path& database_directory,
        InMemoryCatalog& catalog);

    [[nodiscard]] Result<void> CreateTable(const InMemoryTable& table) override;
    [[nodiscard]] Result<RecordId> Insert(const InMemoryTable& table, const Tuple& tuple) override;
    [[nodiscard]] Result<std::vector<Tuple>> Scan(const InMemoryTable& table) const override;

private:
    PersistentStorage(std::filesystem::path database_directory, CatalogStore catalog_store);

    [[nodiscard]] std::filesystem::path TablePath(TableId table_id) const;
    [[nodiscard]] Result<PageManager*> GetTablePages(
        TableId table_id,
        bool create_if_missing) const;

    std::filesystem::path database_directory_;
    CatalogStore catalog_store_;
    mutable std::map<std::uint64_t, std::unique_ptr<PageManager>> table_pages_;
};

}  // namespace kerndb::storage
