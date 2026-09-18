#include "kerndb/storage/page_manager.h"

#include <memory>
#include <utility>

namespace kerndb::storage {

Result<PageManager> PageManager::Open(
    const std::filesystem::path& path,
    bool create_if_missing) {
    auto disk_manager = DiskManager::Open(path, create_if_missing);
    if (!disk_manager.ok()) {
        return disk_manager.status();
    }
    return PageManager{std::move(disk_manager).value()};
}

Result<Page> PageManager::AllocatePage(PageType type) {
    const auto count = PageCount();
    if (!count.ok()) {
        return count.status();
    }
    if (count.value() == PageId::kInvalidValue) {
        return Status::Error(ErrorCode::kResourceExhausted, "page ID space is exhausted");
    }
    const auto page = Page::Create(type, PageId{count.value()});
    if (!page.ok()) {
        return page.status();
    }
    const Status write_status = WritePage(page.value());
    if (!write_status.ok()) {
        return write_status;
    }
    return page.value();
}

Result<Page> PageManager::ReadPage(PageId page_id) const {
    return disk_manager_->ReadPage(page_id);
}

Status PageManager::WritePage(const Page& page) const {
    return disk_manager_->WritePage(page);
}

Result<std::uint64_t> PageManager::PageCount() const {
    return disk_manager_->PageCount();
}

Status PageManager::Flush() const {
    return disk_manager_->Flush();
}

PageManager::PageManager(std::unique_ptr<DiskManager> disk_manager)
    : disk_manager_(std::move(disk_manager)) {}

}  // namespace kerndb::storage
