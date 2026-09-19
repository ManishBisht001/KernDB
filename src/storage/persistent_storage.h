#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>

#include "catalog_store.h"
#include "index_catalog_store.h"
#include "kerndb/storage/table_storage.h"

namespace kerndb::storage {

class PersistentStorage final : public TableStorage {
public:
    [[nodiscard]] static Result<std::unique_ptr<PersistentStorage>> Open(
        const std::filesystem::path& database_directory,
        InMemoryCatalog& catalog);

    [[nodiscard]] Result<void> CreateTable(const InMemoryTable& table) override;
    [[nodiscard]] Result<PageId> CreateIndex(const InMemoryIndex& index) override;
    [[nodiscard]] Result<RecordId> Insert(const InMemoryTable& table, const Tuple& tuple) override;
    [[nodiscard]] Result<std::vector<Tuple>> Scan(const InMemoryTable& table) const override;
    [[nodiscard]] Result<IndexLookup> LookupByIndex(
        const InMemoryTable& table,
        std::size_t column_index,
        std::int64_t key) const override;

private:
    PersistentStorage(
        std::filesystem::path database_directory,
        CatalogStore catalog_store,
        IndexCatalogStore index_catalog_store,
        InMemoryCatalog& catalog);

    [[nodiscard]] std::filesystem::path TablePath(TableId table_id) const;
    [[nodiscard]] std::filesystem::path IndexPath(IndexId index_id) const;
    [[nodiscard]] Result<PageManager*> GetTablePages(
        TableId table_id,
        bool create_if_missing) const;
    [[nodiscard]] Result<PageManager*> GetIndexPages(
        IndexId index_id,
        bool create_if_missing) const;

    std::filesystem::path database_directory_;
    CatalogStore catalog_store_;
    IndexCatalogStore index_catalog_store_;
    InMemoryCatalog* catalog_;
    mutable std::map<std::uint64_t, std::unique_ptr<PageManager>> table_pages_;
    mutable std::map<std::uint64_t, std::unique_ptr<PageManager>> index_pages_;
    std::map<std::uint64_t, InMemoryIndex> indexes_by_id_;
};

}  // namespace kerndb::storage
