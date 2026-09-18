#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "kerndb/result.h"
#include "kerndb/storage/page_manager.h"

namespace kerndb::storage {

class CatalogStore {
public:
    [[nodiscard]] static Result<CatalogStore> Open(const std::filesystem::path& path);

    CatalogStore(CatalogStore&&) noexcept = default;
    CatalogStore& operator=(CatalogStore&&) noexcept = default;
    CatalogStore(const CatalogStore&) = delete;
    CatalogStore& operator=(const CatalogStore&) = delete;

    [[nodiscard]] Result<std::vector<InMemoryTable>> Load() const;
    [[nodiscard]] Status Append(const InMemoryTable& table);

private:
    explicit CatalogStore(PageManager page_manager);

    PageManager page_manager_;
};

}  // namespace kerndb::storage
