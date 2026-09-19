#include "kerndb/storage/buffer_pool.h"

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <utility>

namespace kerndb::storage {
namespace {

[[nodiscard]] Status PoolError(ErrorCode code, std::string message) {
    return Status::Error(code, std::move(message));
}

}  // namespace

PinnedPage::PinnedPage(BufferPoolManager& manager, PageId page_id, Page& page) noexcept
    : manager_(&manager),
      page_id_(page_id),
      page_(&page) {}

PinnedPage::~PinnedPage() {
    static_cast<void>(Release());
}

PinnedPage::PinnedPage(PinnedPage&& other) noexcept
    : manager_(other.manager_),
      page_id_(other.page_id_),
      page_(other.page_),
      dirty_(other.dirty_) {
    other.Reset();
}

PinnedPage& PinnedPage::operator=(PinnedPage&& other) noexcept {
    if (this != &other) {
        static_cast<void>(Release());
        manager_ = other.manager_;
        page_id_ = other.page_id_;
        page_ = other.page_;
        dirty_ = other.dirty_;
        other.Reset();
    }
    return *this;
}

const Page& PinnedPage::page() const {
    assert(page_ != nullptr);
    return *page_;
}

Page& PinnedPage::mutable_page() {
    assert(page_ != nullptr);
    dirty_ = true;
    return *page_;
}

PageId PinnedPage::page_id() const noexcept {
    return page_id_;
}

bool PinnedPage::pinned() const noexcept {
    return page_ != nullptr;
}

Status PinnedPage::Release() {
    if (manager_ == nullptr) {
        return Status::Ok();
    }
    BufferPoolManager* const manager = manager_;
    const PageId page_id = page_id_;
    const bool dirty = dirty_;
    Reset();
    return manager->UnpinPage(page_id, dirty);
}

void PinnedPage::Reset() noexcept {
    manager_ = nullptr;
    page_id_ = PageId{};
    page_ = nullptr;
    dirty_ = false;
}

Result<std::unique_ptr<BufferPoolManager>> BufferPoolManager::Open(
    const std::filesystem::path& path,
    bool create_if_missing,
    std::size_t frame_count,
    MetricsRegistry* metrics) {
    if (frame_count == 0U) {
        return PoolError(ErrorCode::kInvalidArgument, "buffer pool requires at least one frame");
    }
    auto disk_manager = DiskManager::Open(path, create_if_missing);
    if (!disk_manager.ok()) {
        return disk_manager.status();
    }
    return std::unique_ptr<BufferPoolManager>(
        new BufferPoolManager(std::move(disk_manager).value(), frame_count, metrics));
}

BufferPoolManager::~BufferPoolManager() {
    static_cast<void>(FlushAllPages());
}

Result<PinnedPage> BufferPoolManager::FetchPage(PageId page_id) {
    if (!page_id.valid()) {
        IncrementMetric("buffer_pool.failed_fetches");
        return PoolError(ErrorCode::kInvalidArgument, "cannot fetch an invalid page ID");
    }
    const auto resident = page_table_.find(page_id.value());
    if (resident != page_table_.end()) {
        const std::size_t frame_id = resident->second;
        Frame& frame = frames_[frame_id];
        if (!frame.page.has_value()) {
            IncrementMetric("buffer_pool.failed_fetches");
            return PoolError(ErrorCode::kInternal, "buffer pool page table references an empty frame");
        }
        ++frame.pin_count;
        RemoveFromReplacer(frame_id);
        IncrementMetric("buffer_pool.page_hits");
        return PinFrame(frame_id);
    }

    IncrementMetric("buffer_pool.page_misses");
    const auto frame_id = AcquireFrame();
    if (!frame_id.ok()) {
        IncrementMetric("buffer_pool.failed_fetches");
        return frame_id.status();
    }

    // Acquiring a frame only chooses a candidate; it does not evict it. Read
    // the requested page before changing the cache so a corrupt or missing
    // page never needlessly displaces a valid resident page.
    const auto page = disk_manager_->ReadPage(page_id);
    if (!page.ok()) {
        IncrementMetric("buffer_pool.failed_fetches");
        return page.status();
    }

    const Status prepare_status = PrepareFrameForUse(frame_id.value());
    if (!prepare_status.ok()) {
        IncrementMetric("buffer_pool.failed_fetches");
        return prepare_status;
    }

    Frame& frame = frames_[frame_id.value()];
    frame.page = page.value();
    frame.pin_count = 1U;
    frame.dirty = false;
    page_table_.emplace(page_id.value(), frame_id.value());
    return PinFrame(frame_id.value());
}

Result<PinnedPage> BufferPoolManager::NewPage(PageType type) {
    const auto page_count = disk_manager_->PageCount();
    if (!page_count.ok()) {
        IncrementMetric("buffer_pool.failed_allocations");
        return page_count.status();
    }
    if (page_count.value() == PageId::kInvalidValue) {
        IncrementMetric("buffer_pool.failed_allocations");
        return PoolError(ErrorCode::kResourceExhausted, "page ID space is exhausted");
    }
    const PageId page_id{page_count.value()};
    const auto page = Page::Create(type, page_id);
    if (!page.ok()) {
        IncrementMetric("buffer_pool.failed_allocations");
        return page.status();
    }

    const auto frame_id = AcquireFrame();
    if (!frame_id.ok()) {
        IncrementMetric("buffer_pool.failed_allocations");
        return frame_id.status();
    }
    const Status prepare_status = PrepareFrameForUse(frame_id.value());
    if (!prepare_status.ok()) {
        IncrementMetric("buffer_pool.failed_allocations");
        return prepare_status;
    }

    // Materialize a new page immediately. This reserves its physical page ID so
    // a second allocation cannot reuse it before an eventual cache flush.
    const Status write_status = disk_manager_->WritePage(page.value());
    if (!write_status.ok()) {
        IncrementMetric("buffer_pool.failed_allocations");
        return write_status;
    }
    Frame& frame = frames_[frame_id.value()];
    frame.page = page.value();
    frame.pin_count = 1U;
    frame.dirty = false;
    page_table_.emplace(page_id.value(), frame_id.value());
    return PinFrame(frame_id.value());
}

Status BufferPoolManager::FlushPage(PageId page_id) {
    const auto iterator = page_table_.find(page_id.value());
    if (iterator == page_table_.end()) {
        return PoolError(ErrorCode::kNotFound, "page is not resident in the buffer pool")
            .WithContext("page_id", ToString(page_id));
    }
    return FlushFrame(iterator->second);
}

Status BufferPoolManager::FlushAllPages() {
    for (std::size_t frame_id = 0U; frame_id < frames_.size(); ++frame_id) {
        const Status flush_status = FlushFrame(frame_id);
        if (!flush_status.ok()) {
            return flush_status;
        }
    }
    return disk_manager_->Flush();
}

Status BufferPoolManager::DeletePage(PageId page_id) {
    if (!page_id.valid()) {
        return PoolError(ErrorCode::kInvalidArgument, "cannot delete an invalid page ID");
    }
    const auto iterator = page_table_.find(page_id.value());
    if (iterator != page_table_.end()) {
        const std::size_t frame_id = iterator->second;
        Frame& frame = frames_[frame_id];
        if (frame.pin_count != 0U) {
            return PoolError(ErrorCode::kResourceExhausted, "cannot delete a pinned page")
                .WithContext("page_id", ToString(page_id));
        }
    }
    const Status delete_status = disk_manager_->DeletePage(page_id);
    if (!delete_status.ok()) {
        return delete_status;
    }
    if (iterator != page_table_.end()) {
        const std::size_t frame_id = iterator->second;
        Frame& frame = frames_[frame_id];
        RemoveFromReplacer(frame_id);
        page_table_.erase(iterator);
        frame.page.reset();
        frame.pin_count = 0U;
        frame.dirty = false;
    }
    return Status::Ok();
}

Result<std::uint64_t> BufferPoolManager::PageCount() const {
    return disk_manager_->PageCount();
}

std::size_t BufferPoolManager::frame_count() const noexcept {
    return frames_.size();
}

BufferPoolManager::BufferPoolManager(
    std::unique_ptr<DiskManager> disk_manager,
    std::size_t frame_count,
    MetricsRegistry* metrics)
    : disk_manager_(std::move(disk_manager)),
      frames_(frame_count),
      metrics_(metrics) {}

Result<std::size_t> BufferPoolManager::AcquireFrame() {
    for (std::size_t frame_id = 0U; frame_id < frames_.size(); ++frame_id) {
        if (!frames_[frame_id].page.has_value()) {
            return frame_id;
        }
    }
    if (unpinned_lru_.empty()) {
        return PoolError(ErrorCode::kResourceExhausted, "all buffer pool frames are pinned");
    }
    return unpinned_lru_.front();
}

Status BufferPoolManager::PrepareFrameForUse(std::size_t frame_id) {
    Frame& frame = frames_[frame_id];
    if (!frame.page.has_value()) {
        return Status::Ok();
    }
    if (frame.pin_count != 0U) {
        return PoolError(ErrorCode::kInternal, "buffer pool selected a pinned frame for eviction");
    }
    const Status flush_status = FlushFrame(frame_id);
    if (!flush_status.ok()) {
        return flush_status;
    }
    const PageId old_page_id = frame.page->page_id();
    page_table_.erase(old_page_id.value());
    RemoveFromReplacer(frame_id);
    frame.page.reset();
    frame.dirty = false;
    IncrementMetric("buffer_pool.evictions");
    return Status::Ok();
}

Status BufferPoolManager::FlushFrame(std::size_t frame_id) {
    Frame& frame = frames_[frame_id];
    if (!frame.page.has_value() || !frame.dirty) {
        return Status::Ok();
    }
    const Status finalize_status = frame.page->Finalize();
    if (!finalize_status.ok()) {
        return finalize_status;
    }
    const Status write_status = disk_manager_->WritePage(*frame.page);
    if (!write_status.ok()) {
        return write_status;
    }
    frame.dirty = false;
    IncrementMetric("buffer_pool.dirty_flushes");
    return Status::Ok();
}

Status BufferPoolManager::UnpinPage(PageId page_id, bool dirty) {
    const auto iterator = page_table_.find(page_id.value());
    if (iterator == page_table_.end()) {
        return PoolError(ErrorCode::kInternal, "pinned page is absent from the buffer pool")
            .WithContext("page_id", ToString(page_id));
    }
    Frame& frame = frames_[iterator->second];
    if (frame.pin_count == 0U) {
        return PoolError(ErrorCode::kInternal, "buffer pool page pin count underflow")
            .WithContext("page_id", ToString(page_id));
    }
    if (dirty) {
        frame.dirty = true;
    }
    --frame.pin_count;
    if (frame.pin_count == 0U) {
        AddToReplacer(iterator->second);
    }
    return Status::Ok();
}

PinnedPage BufferPoolManager::PinFrame(std::size_t frame_id) {
    Frame& frame = frames_[frame_id];
    assert(frame.page.has_value());
    return PinnedPage{*this, frame.page->page_id(), *frame.page};
}

void BufferPoolManager::RemoveFromReplacer(std::size_t frame_id) {
    const auto iterator = std::find(unpinned_lru_.begin(), unpinned_lru_.end(), frame_id);
    if (iterator != unpinned_lru_.end()) {
        unpinned_lru_.erase(iterator);
    }
}

void BufferPoolManager::AddToReplacer(std::size_t frame_id) {
    RemoveFromReplacer(frame_id);
    unpinned_lru_.push_back(frame_id);
}

void BufferPoolManager::IncrementMetric(std::string_view name) const {
    if (metrics_ != nullptr) {
        static_cast<void>(metrics_->IncrementCounter(name));
    }
}

}  // namespace kerndb::storage
