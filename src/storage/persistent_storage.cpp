#include "storage/persistent_storage.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

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

}  // namespace

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

Result<RecordId> PersistentStorage::Insert(const InMemoryTable& table, const Tuple& tuple) {
    for (const auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id != table.id) {
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
        const std::int64_t key = std::get<std::int64_t>(tuple[index.column_index]);
        const auto existing = tree.value().Search(key);
        if (existing.ok()) {
            return Status::Error(ErrorCode::kAlreadyExists, "duplicate key violates a unique index")
                .WithContext("index", index.name)
                .WithContext("key", std::to_string(key));
        }
        if (existing.status().code() != ErrorCode::kNotFound) {
            return existing.status();
        }
    }

    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    const auto record_id = heap.Insert(table.schema, tuple);
    if (!record_id.ok()) {
        return record_id.status();
    }
    for (auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id != table.id) {
            continue;
        }
        const auto index_pages = GetIndexPages(index.id, false);
        if (!index_pages.ok()) {
            return index_pages.status();
        }
        auto tree = BPlusTree::Open(*index_pages.value(), index.root_page_id);
        if (!tree.ok()) {
            return tree.status();
        }
        const std::int64_t key = std::get<std::int64_t>(tuple[index.column_index]);
        const auto insert_status = tree.value().Insert(key, record_id.value());
        if (!insert_status.ok()) {
            return insert_status.status();
        }
        if (tree.value().root_page_id() != index.root_page_id) {
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
        }
    }
    return record_id.value();
}

Result<std::vector<Tuple>> PersistentStorage::Scan(const InMemoryTable& table) const {
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    return heap.Scan(table.schema);
}

Result<PageId> PersistentStorage::CreateIndex(const InMemoryIndex& index) {
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
