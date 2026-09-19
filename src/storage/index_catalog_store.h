#pragma once

#include <filesystem>
#include <vector>

#include "catalog/catalog.h"
#include "kerndb/result.h"
#include "kerndb/storage/page_manager.h"

namespace kerndb::storage {

// Versioned records make root-page changes durable without altering existing
// Phase 2 table catalog records. Load returns the newest record per IndexId.
class IndexCatalogStore {
public:
    [[nodiscard]] static Result<IndexCatalogStore> Open(const std::filesystem::path& path);

    IndexCatalogStore(IndexCatalogStore&&) noexcept = default;
    IndexCatalogStore& operator=(IndexCatalogStore&&) noexcept = default;
    IndexCatalogStore(const IndexCatalogStore&) = delete;
    IndexCatalogStore& operator=(const IndexCatalogStore&) = delete;

    [[nodiscard]] Result<std::vector<InMemoryIndex>> Load() const;
    [[nodiscard]] Status Append(const InMemoryIndex& index);

private:
    explicit IndexCatalogStore(PageManager page_manager);

    PageManager page_manager_;
};

}  // namespace kerndb::storage
