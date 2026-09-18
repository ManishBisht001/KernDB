#include "kerndb/storage/disk_manager.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <limits>
#include <string>

namespace kerndb::storage {
namespace {

[[nodiscard]] Status IoError(const std::filesystem::path& path, std::string operation) {
    return Status::Error(ErrorCode::kIo, std::move(operation))
        .WithContext("path", path.string());
}

[[nodiscard]] Result<std::uint64_t> PageOffset(PageId page_id) {
    if (!page_id.valid() ||
        page_id.value() > std::numeric_limits<std::uint64_t>::max() / kInitialPageSizeBytes) {
        return Status::Error(ErrorCode::kOutOfRange, "page ID cannot be converted to a file offset");
    }
    return page_id.value() * kInitialPageSizeBytes;
}

}  // namespace

Result<std::unique_ptr<DiskManager>> DiskManager::Open(
    const std::filesystem::path& path,
    bool create_if_missing) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return IoError(path, "failed to inspect page file")
                .WithContext("error", error.message());
        }
        if (!create_if_missing) {
            return Status::Error(ErrorCode::kNotFound, "page file does not exist")
                .WithContext("path", path.string());
        }
        std::ofstream create_file(path, std::ios::binary);
        if (!create_file.is_open()) {
            return IoError(path, "failed to create page file");
        }
        create_file.flush();
        if (!create_file.good()) {
            return IoError(path, "failed to flush newly created page file");
        }
    }

    auto manager = std::unique_ptr<DiskManager>(new DiskManager(path));
    const auto page_count = manager->PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    return manager;
}

Result<std::uint64_t> DiskManager::PageCount() const {
    const auto size = FileSize();
    if (!size.ok()) {
        return size.status();
    }
    if (size.value() % kInitialPageSizeBytes != 0U) {
        return Status::Error(ErrorCode::kCorruption, "page file is not a multiple of the page size")
            .WithContext("path", path_.string())
            .WithContext("file_size", std::to_string(size.value()));
    }
    return static_cast<std::uint64_t>(size.value() / kInitialPageSizeBytes);
}

Result<Page> DiskManager::ReadPage(PageId page_id) const {
    const auto offset = PageOffset(page_id);
    if (!offset.ok()) {
        return offset.status();
    }
    const auto size = FileSize();
    if (!size.ok()) {
        return size.status();
    }
    if (offset.value() > size.value() ||
        size.value() - offset.value() < kInitialPageSizeBytes) {
        return Status::Error(ErrorCode::kOutOfRange, "requested page does not exist in file")
            .WithContext("path", path_.string())
            .WithContext("page_id", ToString(page_id));
    }

    std::ifstream file(path_, std::ios::binary);
    if (!file.is_open()) {
        return IoError(path_, "failed to open page file for reading");
    }
    file.seekg(static_cast<std::streamoff>(offset.value()));
    if (!file.good()) {
        return IoError(path_, "failed to seek to page for reading");
    }
    std::array<std::byte, kInitialPageSizeBytes> bytes{};
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(bytes.size()) || !file.good()) {
        return IoError(path_, "short or failed page read")
            .WithContext("page_id", ToString(page_id));
    }
    return Page::Deserialize(bytes, page_id);
}

Status DiskManager::WritePage(const Page& page) const {
    const auto checked_page = Page::Deserialize(page.bytes(), page.page_id());
    if (!checked_page.ok()) {
        return checked_page.status();
    }
    const auto offset = PageOffset(page.page_id());
    if (!offset.ok()) {
        return offset.status();
    }
    const auto page_count = PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    if (page.page_id().value() > page_count.value()) {
        return Status::Error(ErrorCode::kOutOfRange, "cannot create a gap in a page file")
            .WithContext("page_id", ToString(page.page_id()));
    }

    std::fstream file(path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.is_open()) {
        return IoError(path_, "failed to open page file for writing");
    }
    file.seekp(static_cast<std::streamoff>(offset.value()));
    if (!file.good()) {
        return IoError(path_, "failed to seek to page for writing");
    }
    const std::span<const std::byte> bytes = page.bytes();
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file.good()) {
        return IoError(path_, "short or failed page write")
            .WithContext("page_id", ToString(page.page_id()));
    }
    return Status::Ok();
}

Status DiskManager::Flush() const {
    const auto size = FileSize();
    if (!size.ok()) {
        return size.status();
    }
    return Status::Ok();
}

const std::filesystem::path& DiskManager::path() const noexcept {
    return path_;
}

DiskManager::DiskManager(std::filesystem::path path)
    : path_(std::move(path)) {}

Result<std::uintmax_t> DiskManager::FileSize() const {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path_, error);
    if (error) {
        return IoError(path_, "failed to determine page file size")
            .WithContext("error", error.message());
    }
    return size;
}

}  // namespace kerndb::storage
