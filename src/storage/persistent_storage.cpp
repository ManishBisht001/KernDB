#include "storage/persistent_storage.h"

#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <shared_mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kerndb/storage/bplus_tree.h"
#include "kerndb/storage/database_metadata.h"
#include "kerndb/storage/heap.h"

namespace kerndb::storage {
namespace {

[[nodiscard]] Status FilesystemError(
    const std::filesystem::path& path,
    std::string message,
    const std::error_code& error) {
    return Status::Error(ErrorCode::kIo, std::move(message))
        .WithContext("path", path.string())
        .WithContext("error", error.message());
}

[[nodiscard]] Status EnsureDirectory(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!std::filesystem::create_directories(path, error)) {
            return FilesystemError(path, "failed to create database subdirectory", error);
        }
    }
    if (error || !std::filesystem::is_directory(path, error)) {
        return FilesystemError(path, "database subpath is not a directory", error);
    }
    return Status::Ok();
}

// This is a small logical mutation context. PersistentStorage serializes each
// context while it records compensating actions after completed storage
// changes. It is not a WAL and makes no crash-recovery claim, but it keeps
// failed API mutations from exposing partial heap/index changes in-process.
using UndoActions = std::vector<std::function<Status()>>;

[[nodiscard]] Status RollbackActions(UndoActions& undo_actions) {
    Status first_error = Status::Ok();
    for (auto iterator = undo_actions.rbegin(); iterator != undo_actions.rend(); ++iterator) {
        const Status status = (*iterator)();
        if (!status.ok() && first_error.ok()) {
            first_error = status;
        }
    }
    undo_actions.clear();
    return first_error;
}

class Mutation {
public:
    explicit Mutation(UndoActions* transaction_undo_actions = nullptr)
        : transaction_undo_actions_(transaction_undo_actions) {}

    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;

    ~Mutation() {
        if (!finished_) {
            static_cast<void>(Rollback());
        }
    }

    void AddUndo(std::function<Status()> undo) {
        undo_actions_.push_back(std::move(undo));
    }

    [[nodiscard]] Status Abort(const Status& mutation_error) {
        const Status rollback_status = Rollback();
        if (rollback_status.ok()) {
            return mutation_error;
        }
        return Status::Error(ErrorCode::kInternal, "mutation rollback failed")
            .WithContext("mutation_error", mutation_error.ToString())
            .WithContext("rollback_error", rollback_status.ToString());
    }

    void Commit() {
        if (transaction_undo_actions_ != nullptr) {
            transaction_undo_actions_->insert(
                transaction_undo_actions_->end(),
                std::make_move_iterator(undo_actions_.begin()),
                std::make_move_iterator(undo_actions_.end()));
        }
        undo_actions_.clear();
        finished_ = true;
    }

private:
    [[nodiscard]] Status Rollback() {
        const Status first_error = RollbackActions(undo_actions_);
        finished_ = true;
        return first_error;
    }

    UndoActions undo_actions_;
    UndoActions* transaction_undo_actions_{nullptr};
    bool finished_{false};
};

}  // namespace

#if defined(KERNDB_ENABLE_TEST_HOOKS)
void PersistentStorage::FailNextUpdateAfterRowMutationForTesting() {
    std::scoped_lock lock(mutex_);
    fail_after_next_update_row_for_testing_ = true;
}
#endif

PersistentStorage::~PersistentStorage() {
    std::scoped_lock lock(mutex_);
    for (auto& [transaction_id, undo_actions] : transaction_undo_actions_) {
        static_cast<void>(transaction_id);
        static_cast<void>(RollbackActions(undo_actions));
    }
    transaction_undo_actions_.clear();
}

Status PersistentStorage::BeginTransaction(TransactionId transaction_id) {
    std::scoped_lock lock(mutex_);
    if (!transaction_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "transaction ID is invalid");
    }
    if (!transaction_undo_actions_.emplace(transaction_id.value(), UndoActions{}).second) {
        return Status::Error(ErrorCode::kInvalidArgument, "transaction already has persistent storage state")
            .WithContext("transaction_id", ToString(transaction_id));
    }
    return Status::Ok();
}

Status PersistentStorage::CommitTransaction(TransactionId transaction_id) {
    std::scoped_lock lock(mutex_);
    const auto found = transaction_undo_actions_.find(transaction_id.value());
    if (found == transaction_undo_actions_.end()) {
        return Status::Error(ErrorCode::kInvalidArgument, "there is no active storage transaction to commit")
            .WithContext("transaction_id", ToString(transaction_id));
    }
    transaction_undo_actions_.erase(found);
    return Status::Ok();
}

Status PersistentStorage::RollbackTransaction(TransactionId transaction_id) {
    std::scoped_lock lock(mutex_);
    const auto found = transaction_undo_actions_.find(transaction_id.value());
    if (found == transaction_undo_actions_.end()) {
        return Status::Error(ErrorCode::kInvalidArgument, "there is no active storage transaction to roll back")
            .WithContext("transaction_id", ToString(transaction_id));
    }
    const Status rollback_status = RollbackActions(found->second);
    transaction_undo_actions_.erase(found);
    return rollback_status;
}

Result<PersistentStorage::UndoActions*> PersistentStorage::ResolveUndoActions(
    std::optional<TransactionId> transaction_id) {
    constexpr TransactionId kLegacyTransactionId{0U};
    if (transaction_id.has_value()) {
        if (!transaction_id->valid()) {
            return Status::Error(ErrorCode::kInvalidArgument, "transaction ID is invalid");
        }
        const auto found = transaction_undo_actions_.find(transaction_id->value());
        if (found == transaction_undo_actions_.end()) {
            return Status::Error(ErrorCode::kInvalidArgument, "mutation transaction is not active")
                .WithContext("transaction_id", ToString(*transaction_id));
        }
        return &found->second;
    }

    const auto legacy = transaction_undo_actions_.find(kLegacyTransactionId.value());
    return legacy == transaction_undo_actions_.end() ? nullptr : &legacy->second;
}

Result<std::unique_ptr<PersistentStorage>> PersistentStorage::Open(
    const std::filesystem::path& database_directory,
    InMemoryCatalog& catalog) {
    const auto metadata = OpenOrCreateDatabaseMetadata(database_directory);
    if (!metadata.ok()) {
        return metadata.status();
    }
    static_cast<void>(metadata);
    for (const std::string_view directory : {"tables", "indexes", "wal", "tmp"}) {
        const Status status = EnsureDirectory(database_directory / directory);
        if (!status.ok()) {
            return status;
        }
    }

    auto catalog_store = CatalogStore::Open(database_directory / "catalog.dat");
    if (!catalog_store.ok()) {
        return catalog_store.status();
    }
    auto index_catalog_store = IndexCatalogStore::Open(database_directory / "indexes" / "catalog.dat");
    if (!index_catalog_store.ok()) {
        return index_catalog_store.status();
    }
    auto storage = std::unique_ptr<PersistentStorage>(
        new PersistentStorage(
            database_directory,
            std::move(catalog_store).value(),
            std::move(index_catalog_store).value(),
            catalog));
    const auto tables = storage->catalog_store_.Load();
    if (!tables.ok()) {
        return tables.status();
    }
    for (const InMemoryTable& table : tables.value()) {
        const std::filesystem::path table_path = storage->TablePath(table.id);
        std::error_code error;
        if (!std::filesystem::exists(table_path, error)) {
            if (error) {
                return FilesystemError(table_path, "failed to inspect table file", error);
            }
            return Status::Error(ErrorCode::kNotFound, "catalog references a missing table file")
                .WithContext("table_id", ToString(table.id))
                .WithContext("path", table_path.string());
        }
        const Status load_status = catalog.LoadTable(table.id, table.name, table.schema);
        if (!load_status.ok()) {
            return load_status;
        }
    }
    const auto indexes = storage->index_catalog_store_.Load();
    if (!indexes.ok()) {
        return indexes.status();
    }
    for (const InMemoryIndex& index : indexes.value()) {
        const auto table = catalog.FindTableById(index.table_id);
        if (!table.ok() || index.column_index >= table.value()->schema.size() ||
            table.value()->schema[index.column_index].type != ColumnType::kInt) {
            return Status::Error(ErrorCode::kCorruption, "index catalog references an invalid INT column")
                .WithContext("index", index.name);
        }
        const std::filesystem::path index_path = storage->IndexPath(index.id);
        std::error_code error;
        if (!std::filesystem::exists(index_path, error)) {
            if (error) {
                return FilesystemError(index_path, "failed to inspect index file", error);
            }
            return Status::Error(ErrorCode::kNotFound, "catalog references a missing index file")
                .WithContext("index", index.name)
                .WithContext("path", index_path.string());
        }
        const auto index_pages = storage->GetIndexPages(index.id, false);
        if (!index_pages.ok()) {
            return index_pages.status();
        }
        const auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
        if (!tree.ok()) {
            return tree.status();
        }
        const Status load_status = catalog.LoadIndex(index);
        if (!load_status.ok()) {
            return load_status;
        }
        storage->indexes_by_id_.emplace(index.id.value(), index);
    }
    return storage;
}

Result<void> PersistentStorage::CreateTable(const InMemoryTable& table) {
    std::scoped_lock lock(mutex_);
    const std::filesystem::path table_path = TablePath(table.id);
    std::error_code error;
    if (std::filesystem::exists(table_path, error)) {
        return Status::Error(ErrorCode::kAlreadyExists, "table storage file already exists")
            .WithContext("path", table_path.string());
    }
    if (error) {
        return FilesystemError(table_path, "failed to inspect table storage file", error);
    }
    const auto pages = GetTablePages(table.id, true);
    if (!pages.ok()) {
        return pages.status();
    }
    const Status flush_status = pages.value()->Flush();
    if (!flush_status.ok()) {
        return flush_status;
    }
    const Status catalog_status = catalog_store_.Append(table);
    if (!catalog_status.ok()) {
        table_pages_.erase(table.id.value());
        std::error_code cleanup_error;
        std::filesystem::remove(table_path, cleanup_error);
        return catalog_status;
    }
    return Result<void>{};
}

Result<RecordId> PersistentStorage::Insert(
    const InMemoryTable& table,
    const Tuple& tuple,
    std::optional<TransactionId> transaction_id) {
    std::scoped_lock lock(mutex_);
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};

    const std::vector<InMemoryIndex*> indexes = IndexesForTable(table.id);
    for (const InMemoryIndex* index : indexes) {
        const auto index_pages = GetIndexPages(index->id, false);
        if (!index_pages.ok()) {
            return index_pages.status();
        }
        const auto tree = BPlusTree::Open(*index_pages.value(), index->root_page_id);
        if (!tree.ok()) {
            return tree.status();
        }
        const std::int64_t key = std::get<std::int64_t>(tuple[index->column_index]);
        const auto existing = tree.value().Search(key);
        if (existing.ok()) {
            return Status::Error(ErrorCode::kAlreadyExists, "duplicate key violates a unique index")
                .WithContext("index", index->name)
                .WithContext("key", std::to_string(key));
        }
        if (existing.status().code() != ErrorCode::kNotFound) {
            return existing.status();
        }
    }

    const auto undo_actions = ResolveUndoActions(transaction_id);
    if (!undo_actions.ok()) {
        return undo_actions.status();
    }
    Mutation mutation{undo_actions.value()};
    const auto record_id = heap.Insert(table.schema, tuple);
    if (!record_id.ok()) {
        return record_id.status();
    }
    mutation.AddUndo([table_pages = pages.value(), inserted_record_id = record_id.value()] {
        TableHeap undo_heap{*table_pages};
        return undo_heap.Erase(inserted_record_id);
    });

    for (InMemoryIndex* index : indexes) {
        const std::int64_t key = std::get<std::int64_t>(tuple[index->column_index]);
        const Status insert_status = InsertIndexEntry(*index, key, record_id.value());
        if (!insert_status.ok()) {
            return mutation.Abort(insert_status);
        }
        mutation.AddUndo([this, index_id = index->id, key, inserted_record_id = record_id.value()] {
            const auto found = indexes_by_id_.find(index_id.value());
            if (found == indexes_by_id_.end()) {
                return Status::Error(ErrorCode::kInternal, "rollback index metadata is missing");
            }
            return RemoveIndexEntry(found->second, key, inserted_record_id);
        });
    }

    mutation.Commit();
    return record_id.value();
}

Result<std::uint64_t> PersistentStorage::UpdateMatching(
    const InMemoryTable& table,
    std::size_t column_index,
    const Value& value,
    std::size_t predicate_column_index,
    const Value& predicate_value,
    std::optional<TransactionId> transaction_id) {
    std::scoped_lock lock(mutex_);
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    const auto records = heap.ScanWithRecordIds(table.schema);
    if (!records.ok()) {
        return records.status();
    }

    std::vector<std::pair<RecordId, Tuple>> matches;
    for (const auto& record : records.value()) {
        if (ValuesEqual(record.second[predicate_column_index], predicate_value)) {
            matches.push_back(record);
        }
    }

    const std::vector<InMemoryIndex*> indexes = IndexesForTable(table.id);
    for (const InMemoryIndex* index : indexes) {
        std::set<std::int64_t> final_keys;
        for (const auto& [record_id, original_tuple] : records.value()) {
            static_cast<void>(record_id);
            Tuple final_tuple = original_tuple;
            if (ValuesEqual(original_tuple[predicate_column_index], predicate_value)) {
                final_tuple[column_index] = value;
            }
            const std::int64_t key = std::get<std::int64_t>(final_tuple[index->column_index]);
            if (!final_keys.insert(key).second) {
                return Status::Error(ErrorCode::kAlreadyExists, "duplicate key violates a unique index")
                    .WithContext("index", index->name)
                    .WithContext("key", std::to_string(key));
            }
        }
    }

    for (const auto& [record_id, original_tuple] : matches) {
        for (const InMemoryIndex* index : indexes) {
            const std::int64_t old_key = std::get<std::int64_t>(original_tuple[index->column_index]);
            const Status verification = VerifyIndexEntry(*index, old_key, record_id);
            if (!verification.ok()) {
                return verification;
            }
        }
    }

    const auto undo_actions = ResolveUndoActions(transaction_id);
    if (!undo_actions.ok()) {
        return undo_actions.status();
    }
    Mutation mutation{undo_actions.value()};
    for (const auto& [old_record_id, original_tuple] : matches) {
        Tuple replacement = original_tuple;
        replacement[column_index] = value;
        const auto new_record_id = heap.Insert(table.schema, replacement);
        if (!new_record_id.ok()) {
            return mutation.Abort(new_record_id.status());
        }
        mutation.AddUndo([table_pages = pages.value(), inserted_record_id = new_record_id.value()] {
            TableHeap undo_heap{*table_pages};
            return undo_heap.Erase(inserted_record_id);
        });

        for (InMemoryIndex* index : indexes) {
            const std::int64_t old_key = std::get<std::int64_t>(original_tuple[index->column_index]);
            const std::int64_t new_key = std::get<std::int64_t>(replacement[index->column_index]);
            const Status remove_status = RemoveIndexEntry(*index, old_key, old_record_id);
            if (!remove_status.ok()) {
                return mutation.Abort(remove_status);
            }
            mutation.AddUndo([this, index_id = index->id, old_key, old_record_id] {
                const auto found = indexes_by_id_.find(index_id.value());
                if (found == indexes_by_id_.end()) {
                    return Status::Error(ErrorCode::kInternal, "rollback index metadata is missing");
                }
                return InsertIndexEntry(found->second, old_key, old_record_id);
            });

            const Status insert_status = InsertIndexEntry(*index, new_key, new_record_id.value());
            if (!insert_status.ok()) {
                return mutation.Abort(insert_status);
            }
            mutation.AddUndo([this, index_id = index->id, new_key, inserted_record_id = new_record_id.value()] {
                const auto found = indexes_by_id_.find(index_id.value());
                if (found == indexes_by_id_.end()) {
                    return Status::Error(ErrorCode::kInternal, "rollback index metadata is missing");
                }
                return RemoveIndexEntry(found->second, new_key, inserted_record_id);
            });
        }

        const Status erase_status = heap.Erase(old_record_id);
        if (!erase_status.ok()) {
            return mutation.Abort(erase_status);
        }
        mutation.AddUndo([table_pages = pages.value(), old_record_id] {
            TableHeap undo_heap{*table_pages};
            return undo_heap.Restore(old_record_id);
        });

#if defined(KERNDB_ENABLE_TEST_HOOKS)
        // Trigger only after the replacement row is indexed and the original
        // heap slot is tombstoned, with an undo action registered for each.
        if (fail_after_next_update_row_for_testing_) {
            fail_after_next_update_row_for_testing_ = false;
            return mutation.Abort(Status::Error(
                ErrorCode::kIo,
                "injected failure after persistent update row mutation"));
        }
#endif
    }

    mutation.Commit();
    return static_cast<std::uint64_t>(matches.size());
}

Result<std::uint64_t> PersistentStorage::DeleteMatching(
    const InMemoryTable& table,
    std::size_t predicate_column_index,
    const Value& predicate_value,
    std::optional<TransactionId> transaction_id) {
    std::scoped_lock lock(mutex_);
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    const auto records = heap.ScanWithRecordIds(table.schema);
    if (!records.ok()) {
        return records.status();
    }

    std::vector<std::pair<RecordId, Tuple>> matches;
    for (const auto& record : records.value()) {
        if (ValuesEqual(record.second[predicate_column_index], predicate_value)) {
            matches.push_back(record);
        }
    }

    const std::vector<InMemoryIndex*> indexes = IndexesForTable(table.id);
    for (const auto& [record_id, tuple] : matches) {
        for (const InMemoryIndex* index : indexes) {
            const std::int64_t key = std::get<std::int64_t>(tuple[index->column_index]);
            const Status verification = VerifyIndexEntry(*index, key, record_id);
            if (!verification.ok()) {
                return verification;
            }
        }
    }

    const auto undo_actions = ResolveUndoActions(transaction_id);
    if (!undo_actions.ok()) {
        return undo_actions.status();
    }
    Mutation mutation{undo_actions.value()};
    for (const auto& [record_id, tuple] : matches) {
        for (InMemoryIndex* index : indexes) {
            const std::int64_t key = std::get<std::int64_t>(tuple[index->column_index]);
            const Status remove_status = RemoveIndexEntry(*index, key, record_id);
            if (!remove_status.ok()) {
                return mutation.Abort(remove_status);
            }
            mutation.AddUndo([this, index_id = index->id, key, record_id] {
                const auto found = indexes_by_id_.find(index_id.value());
                if (found == indexes_by_id_.end()) {
                    return Status::Error(ErrorCode::kInternal, "rollback index metadata is missing");
                }
                return InsertIndexEntry(found->second, key, record_id);
            });
        }
        const Status erase_status = heap.Erase(record_id);
        if (!erase_status.ok()) {
            return mutation.Abort(erase_status);
        }
        mutation.AddUndo([table_pages = pages.value(), record_id] {
            TableHeap undo_heap{*table_pages};
            return undo_heap.Restore(record_id);
        });
    }

    mutation.Commit();
    return static_cast<std::uint64_t>(matches.size());
}

Result<std::vector<Tuple>> PersistentStorage::Scan(const InMemoryTable& table) const {
    std::shared_lock lock(mutex_);
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    return heap.Scan(table.schema);
}

Result<PageId> PersistentStorage::CreateIndex(const InMemoryIndex& index) {
    std::scoped_lock lock(mutex_);
    if (!index.id.valid() || index.root_page_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "new index metadata is invalid");
    }
    const std::filesystem::path index_path = IndexPath(index.id);
    std::error_code error;
    if (std::filesystem::exists(index_path, error)) {
        return Status::Error(ErrorCode::kAlreadyExists, "index storage file already exists")
            .WithContext("path", index_path.string());
    }
    if (error) {
        return FilesystemError(index_path, "failed to inspect index storage file", error);
    }
    const auto index_pages = GetIndexPages(index.id, true);
    if (!index_pages.ok()) {
        return index_pages.status();
    }
    auto cleanup = [&] {
        index_pages_.erase(index.id.value());
        std::error_code cleanup_error;
        std::filesystem::remove(index_path, cleanup_error);
    };
    auto tree = BPlusTree::Create(*index_pages.value());
    if (!tree.ok()) {
        cleanup();
        return tree.status();
    }
    const auto table = catalog_->FindTableById(index.table_id);
    if (!table.ok()) {
        cleanup();
        return table.status();
    }
    const auto table_pages = GetTablePages(index.table_id, false);
    if (!table_pages.ok()) {
        cleanup();
        return table_pages.status();
    }
    TableHeap heap{*table_pages.value()};
    const auto records = heap.ScanWithRecordIds(table.value()->schema);
    if (!records.ok()) {
        cleanup();
        return records.status();
    }
    for (const auto& [record_id, tuple] : records.value()) {
        const std::int64_t key = std::get<std::int64_t>(tuple[index.column_index]);
        const auto insert_status = tree.value().Insert(key, record_id);
        if (!insert_status.ok()) {
            cleanup();
            return insert_status.status();
        }
    }
    InMemoryIndex persisted = index;
    persisted.root_page_id = tree.value().root_page_id();
    const Status catalog_status = index_catalog_store_.Append(persisted);
    if (!catalog_status.ok()) {
        cleanup();
        return catalog_status;
    }
    indexes_by_id_.emplace(persisted.id.value(), persisted);
    return persisted.root_page_id;
}

Result<IndexLookup> PersistentStorage::LookupByIndex(
    const InMemoryTable& table,
    std::size_t column_index,
    std::int64_t key) const {
    std::shared_lock lock(mutex_);
    for (const auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id != table.id || index.column_index != column_index) {
            continue;
        }
        const auto index_pages = GetIndexPages(index.id, false);
        if (!index_pages.ok()) {
            return index_pages.status();
        }
        const auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
        if (!tree.ok()) {
            return tree.status();
        }
        const auto record_id = tree.value().Search(key);
        if (!record_id.ok()) {
            if (record_id.status().code() == ErrorCode::kNotFound) {
                return IndexLookup{.index_available = true, .tuple = std::nullopt};
            }
            return record_id.status();
        }
        const auto table_pages = GetTablePages(table.id, false);
        if (!table_pages.ok()) {
            return table_pages.status();
        }
        TableHeap heap{*table_pages.value()};
        const auto tuple = heap.Read(table.schema, record_id.value());
        if (!tuple.ok()) {
            return tuple.status();
        }
        return IndexLookup{.index_available = true, .tuple = tuple.value()};
    }
    return IndexLookup{.index_available = false, .tuple = std::nullopt};
}

std::vector<InMemoryIndex*> PersistentStorage::IndexesForTable(TableId table_id) {
    std::vector<InMemoryIndex*> indexes;
    for (auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id == table_id) {
            indexes.push_back(&index);
        }
    }
    return indexes;
}

Status PersistentStorage::VerifyIndexEntry(
    const InMemoryIndex& index,
    std::int64_t key,
    RecordId record_id) const {
    const auto index_pages = GetIndexPages(index.id, false);
    if (!index_pages.ok()) {
        return index_pages.status();
    }
    const auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
    if (!tree.ok()) {
        return tree.status();
    }
    const auto found = tree.value().Search(key);
    if (!found.ok()) {
        return found.status();
    }
    if (found.value() != record_id) {
        return Status::Error(ErrorCode::kCorruption, "index key refers to an unexpected record ID")
            .WithContext("index", index.name)
            .WithContext("key", std::to_string(key));
    }
    return Status::Ok();
}

Status PersistentStorage::InsertIndexEntry(
    InMemoryIndex& index,
    std::int64_t key,
    RecordId record_id) {
    const auto index_pages = GetIndexPages(index.id, false);
    if (!index_pages.ok()) {
        return index_pages.status();
    }
    auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
    if (!tree.ok()) {
        return tree.status();
    }
    const auto insert_status = tree.value().Insert(key, record_id);
    if (!insert_status.ok()) {
        return insert_status.status();
    }
    if (tree.value().root_page_id() == index.root_page_id) {
        return Status::Ok();
    }

    InMemoryIndex updated = index;
    updated.root_page_id = tree.value().root_page_id();
    const Status persist_status = index_catalog_store_.Append(updated);
    if (!persist_status.ok()) {
        return persist_status;
    }
    const Status catalog_status = catalog_->UpdateIndexRoot(updated.id, updated.root_page_id);
    if (!catalog_status.ok()) {
        return catalog_status;
    }
    index = std::move(updated);
    return Status::Ok();
}

Status PersistentStorage::RemoveIndexEntry(
    const InMemoryIndex& index,
    std::int64_t key,
    RecordId record_id) const {
    const auto index_pages = GetIndexPages(index.id, false);
    if (!index_pages.ok()) {
        return index_pages.status();
    }
    auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
    if (!tree.ok()) {
        return tree.status();
    }
    const auto remove_status = tree.value().Remove(key, record_id);
    if (!remove_status.ok()) {
        return remove_status.status();
    }
    return Status::Ok();
}

PersistentStorage::PersistentStorage(
    std::filesystem::path database_directory,
    CatalogStore catalog_store,
    IndexCatalogStore index_catalog_store,
    InMemoryCatalog& catalog)
    : database_directory_(std::move(database_directory)),
      catalog_store_(std::move(catalog_store)),
      index_catalog_store_(std::move(index_catalog_store)),
      catalog_(&catalog) {}

std::filesystem::path PersistentStorage::TablePath(TableId table_id) const {
    return database_directory_ / "tables" / (ToString(table_id) + ".dat");
}

std::filesystem::path PersistentStorage::IndexPath(IndexId index_id) const {
    return database_directory_ / "indexes" / (ToString(index_id) + ".dat");
}

Result<PageManager*> PersistentStorage::GetTablePages(
    TableId table_id,
    bool create_if_missing) const {
    std::scoped_lock cache_lock(page_cache_mutex_);
    const auto existing = table_pages_.find(table_id.value());
    if (existing != table_pages_.end()) {
        return existing->second.get();
    }
    auto opened = PageManager::Open(TablePath(table_id), create_if_missing);
    if (!opened.ok()) {
        return opened.status();
    }
    auto page_manager = std::make_unique<PageManager>(std::move(opened).value());
    const auto [iterator, inserted] = table_pages_.emplace(table_id.value(), std::move(page_manager));
    if (!inserted) {
        return Status::Error(ErrorCode::kInternal, "table page manager was inserted concurrently");
    }
    return iterator->second.get();
}

Result<PageManager*> PersistentStorage::GetIndexPages(
    IndexId index_id,
    bool create_if_missing) const {
    std::scoped_lock cache_lock(page_cache_mutex_);
    const auto existing = index_pages_.find(index_id.value());
    if (existing != index_pages_.end()) {
        return existing->second.get();
    }
    auto opened = PageManager::Open(IndexPath(index_id), create_if_missing);
    if (!opened.ok()) {
        return opened.status();
    }
    auto page_manager = std::make_unique<PageManager>(std::move(opened).value());
    const auto [iterator, inserted] = index_pages_.emplace(index_id.value(), std::move(page_manager));
    if (!inserted) {
        return Status::Error(ErrorCode::kInternal, "index page manager was inserted concurrently");
    }
    return iterator->second.get();
}

Result<std::unique_ptr<PersistentStorage>> OpenPersistentStorage(
    const std::filesystem::path& database_directory,
    InMemoryCatalog& catalog) {
    return PersistentStorage::Open(database_directory, catalog);
}

}  // namespace kerndb::storage
