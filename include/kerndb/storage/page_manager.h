#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "kerndb/result.h"
#include "kerndb/storage/disk_manager.h"

namespace kerndb::storage {

class PageManager {
public:
    [[nodiscard]] static Result<PageManager> Open(
        const std::filesystem::path& path,
        bool create_if_missing);

    PageManager(PageManager&&) noexcept = default;
    PageManager& operator=(PageManager&&) noexcept = default;
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    [[nodiscard]] Result<Page> AllocatePage(PageType type);
    [[nodiscard]] Result<Page> ReadPage(PageId page_id) const;
    [[nodiscard]] Status WritePage(const Page& page) const;
    [[nodiscard]] Result<std::uint64_t> PageCount() const;
    [[nodiscard]] Status Flush() const;

private:
    explicit PageManager(std::unique_ptr<DiskManager> disk_manager);

    std::unique_ptr<DiskManager> disk_manager_;
};

}  // namespace kerndb::storage
