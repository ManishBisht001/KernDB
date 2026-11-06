#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "catalog_store.h"
#include "index_catalog_store.h"
#include "kerndb/storage/table_storage.h"

namespace kerndb::storage {

class PersistentStorage final : public TableStorage {
public:
#if defined(KERNDB_ENABLE_TEST_HOOKS)
    void FailNextUpdateAfterRowMutationForTesting() noexcept;
#endif

    [[nodiscard]] static Result<std::unique_ptr<PersistentStorage>> Open(
        const std::filesystem::path& database_directory,
        InMemoryCatalog& catalog);
    ~PersistentStorage() override;

    Status BeginTransaction() override;
    Status CommitTransaction() override;
    Status RollbackTransaction() override;

    [[nodiscard]] Result<void> CreateTable(const InMemoryTable& table) override;
    [[nodiscard]] Result<PageId> CreateIndex(const InMemoryIndex& index) override;
    [[nodiscard]] Result<RecordId> Insert(const InMemoryTable& table, const Tuple& tuple) override;
    [[nodiscard]] Result<std::uint64_t> UpdateMatching(
        const InMemoryTable& table,
        std::size_t column_index,
        const Value& value,
        std::size_t predicate_column_index,
        const Value& predicate_value) override;
    [[nodiscard]] Result<std::uint64_t> DeleteMatching(
        const InMemoryTable& table,
        std::size_t predicate_column_index,
        const Value& predicate_value) override;
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
    [[nodiscard]] std::vector<InMemoryIndex*> IndexesForTable(TableId table_id);
    [[nodiscard]] Status VerifyIndexEntry(
        const InMemoryIndex& index,
        std::int64_t key,
        RecordId record_id) const;
    [[nodiscard]] Status InsertIndexEntry(
        InMemoryIndex& index,
        std::int64_t key,
        RecordId record_id);
    [[nodiscard]] Status RemoveIndexEntry(
        const InMemoryIndex& index,
        std::int64_t key,
        RecordId record_id) const;

    std::filesystem::path database_directory_;
    CatalogStore catalog_store_;
    IndexCatalogStore index_catalog_store_;
    InMemoryCatalog* catalog_;
    mutable std::map<std::uint64_t, std::unique_ptr<PageManager>> table_pages_;
    mutable std::map<std::uint64_t, std::unique_ptr<PageManager>> index_pages_;
    std::map<std::uint64_t, InMemoryIndex> indexes_by_id_;
    std::vector<std::function<Status()>> transaction_undo_actions_;
    bool transaction_active_{false};
#if defined(KERNDB_ENABLE_TEST_HOOKS)
    bool fail_after_next_update_row_for_testing_{false};
#endif
};

}  // namespace kerndb::storage
