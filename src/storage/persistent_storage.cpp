#include "storage/persistent_storage.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

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
    auto storage = std::unique_ptr<PersistentStorage>(
        new PersistentStorage(database_directory, std::move(catalog_store).value()));
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
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    return heap.Insert(table.schema, tuple);
}

Result<std::vector<Tuple>> PersistentStorage::Scan(const InMemoryTable& table) const {
    const auto pages = GetTablePages(table.id, false);
    if (!pages.ok()) {
        return pages.status();
    }
    TableHeap heap{*pages.value()};
    return heap.Scan(table.schema);
}

PersistentStorage::PersistentStorage(
    std::filesystem::path database_directory,
    CatalogStore catalog_store)
    : database_directory_(std::move(database_directory)),
      catalog_store_(std::move(catalog_store)) {}

std::filesystem::path PersistentStorage::TablePath(TableId table_id) const {
    return database_directory_ / "tables" / (ToString(table_id) + ".dat");
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

Result<std::unique_ptr<PersistentStorage>> OpenPersistentStorage(
    const std::filesystem::path& database_directory,
    InMemoryCatalog& catalog) {
    return PersistentStorage::Open(database_directory, catalog);
}

}  // namespace kerndb::storage
