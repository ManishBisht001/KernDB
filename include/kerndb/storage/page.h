#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "kerndb/ids.h"
#include "kerndb/options.h"
#include "kerndb/result.h"

namespace kerndb::storage {

inline constexpr std::uint32_t kPageMagic = 0x4B504147U;
inline constexpr std::size_t kPageHeaderSize = 40U;

enum class PageType : std::uint16_t {
    kCatalog = 1U,
    kHeap = 2U,
    kIndex = 3U,
};

class Page {
public:
    [[nodiscard]] static Result<Page> Create(PageType type, PageId page_id);
    [[nodiscard]] static Result<Page> Deserialize(
        std::span<const std::byte> bytes,
        std::optional<PageId> expected_page_id = std::nullopt);

    [[nodiscard]] PageId page_id() const noexcept;
    [[nodiscard]] PageType type() const noexcept;
    [[nodiscard]] std::uint64_t page_lsn() const noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    [[nodiscard]] std::span<std::byte> mutable_bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;
    [[nodiscard]] std::span<std::byte> mutable_payload() noexcept;
    [[nodiscard]] Status Finalize();

private:
    [[nodiscard]] static Result<PageType> DecodePageType(std::uint16_t raw_type);
    [[nodiscard]] static std::uint32_t Checksum(std::span<const std::byte> bytes) noexcept;

    std::array<std::byte, kInitialPageSizeBytes> bytes_{};
};

}  // namespace kerndb::storage
