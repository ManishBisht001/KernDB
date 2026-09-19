#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "kerndb/result.h"
#include "kerndb/storage/buffer_pool.h"

namespace kerndb::storage {

class PageManager {
public:
    [[nodiscard]] static Result<PageManager> Open(
        const std::filesystem::path& path,
        bool create_if_missing,
        std::size_t buffer_pool_frames = kDefaultBufferPoolFrames,
        MetricsRegistry* metrics = nullptr);

    PageManager(PageManager&&) noexcept = default;
    PageManager& operator=(PageManager&&) noexcept = default;
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    [[nodiscard]] Result<Page> AllocatePage(PageType type);
    [[nodiscard]] Result<Page> ReadPage(PageId page_id) const;
    [[nodiscard]] Status WritePage(const Page& page) const;
    [[nodiscard]] Result<std::uint64_t> PageCount() const;
    [[nodiscard]] Status Flush() const;
    [[nodiscard]] Status DeletePage(PageId page_id);
    [[nodiscard]] BufferPoolManager& buffer_pool() noexcept;
    [[nodiscard]] const BufferPoolManager& buffer_pool() const noexcept;

private:
    explicit PageManager(std::unique_ptr<BufferPoolManager> buffer_pool);

    std::unique_ptr<BufferPoolManager> buffer_pool_;
};

}  // namespace kerndb::storage
