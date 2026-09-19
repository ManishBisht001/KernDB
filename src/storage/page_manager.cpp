#include "kerndb/storage/page_manager.h"

#include <memory>
#include <utility>

namespace kerndb::storage {

Result<PageManager> PageManager::Open(
    const std::filesystem::path& path,
    bool create_if_missing,
    std::size_t buffer_pool_frames,
    MetricsRegistry* metrics) {
    auto buffer_pool = BufferPoolManager::Open(
        path,
        create_if_missing,
        buffer_pool_frames,
        metrics);
    if (!buffer_pool.ok()) {
        return buffer_pool.status();
    }
    return PageManager{std::move(buffer_pool).value()};
}

Result<Page> PageManager::AllocatePage(PageType type) {
    const auto page = buffer_pool_->NewPage(type);
    if (!page.ok()) {
        return page.status();
    }
    return page.value().page();
}

Result<Page> PageManager::ReadPage(PageId page_id) const {
    const auto page = buffer_pool_->FetchPage(page_id);
    if (!page.ok()) {
        return page.status();
    }
    return page.value().page();
}

Status PageManager::WritePage(const Page& page) const {
    auto pinned_page = buffer_pool_->FetchPage(page.page_id());
    if (!pinned_page.ok()) {
        return pinned_page.status();
    }
    if (pinned_page.value().page_id() != page.page_id()) {
        return Status::Error(ErrorCode::kInternal, "buffer pool returned a mismatched page ID");
    }
    pinned_page.value().mutable_page() = page;
    const Status unpin_status = pinned_page.value().Release();
    if (!unpin_status.ok()) {
        return unpin_status;
    }

    // Preserve the Phase 2 WritePage contract: a successful public write is
    // immediately visible to independently opened page managers. Storage that
    // needs deferred write-back can use the guarded buffer_pool() API.
    return buffer_pool_->FlushPage(page.page_id());
}

Result<std::uint64_t> PageManager::PageCount() const {
    return buffer_pool_->PageCount();
}

Status PageManager::Flush() const {
    return buffer_pool_->FlushAllPages();
}

Status PageManager::DeletePage(PageId page_id) {
    return buffer_pool_->DeletePage(page_id);
}

BufferPoolManager& PageManager::buffer_pool() noexcept {
    return *buffer_pool_;
}

const BufferPoolManager& PageManager::buffer_pool() const noexcept {
    return *buffer_pool_;
}

PageManager::PageManager(std::unique_ptr<BufferPoolManager> buffer_pool)
    : buffer_pool_(std::move(buffer_pool)) {}

}  // namespace kerndb::storage
