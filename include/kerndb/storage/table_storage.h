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

    // Transaction ID zero is reserved for this compatibility surface. New
    // sessions pass their own valid transaction IDs explicitly.
    virtual Status BeginTransaction(TransactionId transaction_id) = 0;
    virtual Status CommitTransaction(TransactionId transaction_id) = 0;
    virtual Status RollbackTransaction(TransactionId transaction_id) = 0;
    [[nodiscard]] Status BeginTransaction() { return BeginTransaction(TransactionId{0U}); }
    [[nodiscard]] Status CommitTransaction() { return CommitTransaction(TransactionId{0U}); }
    [[nodiscard]] Status RollbackTransaction() { return RollbackTransaction(TransactionId{0U}); }

    virtual Result<void> CreateTable(const InMemoryTable& table) = 0;
    virtual Result<PageId> CreateIndex(const InMemoryIndex& index) = 0;
    virtual Result<RecordId> Insert(
        const InMemoryTable& table,
        const Tuple& tuple,
        std::optional<TransactionId> transaction_id = std::nullopt) = 0;
    virtual Result<std::uint64_t> UpdateMatching(
        const InMemoryTable& table,
        std::size_t column_index,
        const Value& value,
        std::size_t predicate_column_index,
        const Value& predicate_value,
        std::optional<TransactionId> transaction_id = std::nullopt) = 0;
    virtual Result<std::uint64_t> DeleteMatching(
        const InMemoryTable& table,
        std::size_t predicate_column_index,
        const Value& predicate_value,
        std::optional<TransactionId> transaction_id = std::nullopt) = 0;
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
