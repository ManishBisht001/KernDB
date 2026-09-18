#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/storage/page.h"

namespace kerndb::storage {

class DiskManager {
public:
    [[nodiscard]] static Result<std::unique_ptr<DiskManager>> Open(
        const std::filesystem::path& path,
        bool create_if_missing);

    [[nodiscard]] Result<std::uint64_t> PageCount() const;
    [[nodiscard]] Result<Page> ReadPage(PageId page_id) const;
    [[nodiscard]] Status WritePage(const Page& page) const;
    [[nodiscard]] Status Flush() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    explicit DiskManager(std::filesystem::path path);

    [[nodiscard]] Result<std::uintmax_t> FileSize() const;

    std::filesystem::path path_;
};

}  // namespace kerndb::storage
