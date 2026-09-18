#include "kerndb/storage/page.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kerndb::storage {
namespace {

constexpr std::size_t kMagicOffset = 0U;
constexpr std::size_t kVersionOffset = 4U;
constexpr std::size_t kTypeOffset = 8U;
constexpr std::size_t kHeaderSizeOffset = 10U;
constexpr std::size_t kPageIdOffset = 12U;
constexpr std::size_t kPageLsnOffset = 20U;
constexpr std::size_t kChecksumOffset = 28U;
constexpr std::size_t kFlagsOffset = 32U;
constexpr std::size_t kReservedOffset = 36U;

void WriteUInt16At(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

void WriteUInt32At(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

void WriteUInt64At(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            static_cast<unsigned char>((value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] std::uint16_t ReadUInt16At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint16_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        const auto byte = static_cast<std::uint16_t>(
            std::to_integer<unsigned char>(bytes[offset + index]));
        result |= static_cast<std::uint16_t>(byte << (index * 8U));
    }
    return result;
}

[[nodiscard]] std::uint32_t ReadUInt32At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        result |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8U);
    }
    return result;
}

[[nodiscard]] std::uint64_t ReadUInt64At(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t result = 0U;
    for (std::size_t index = 0U; index < sizeof(result); ++index) {
        result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + index]))
                  << (index * 8U);
    }
    return result;
}

[[nodiscard]] Status Corruption(std::string message) {
    return Status::Error(ErrorCode::kCorruption, std::move(message));
}

}  // namespace

Result<Page> Page::Create(PageType type, PageId page_id) {
    if (!page_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "page ID is invalid");
    }
    Page page;
    const auto type_status = DecodePageType(static_cast<std::uint16_t>(type));
    if (!type_status.ok()) {
        return type_status.status();
    }
    std::fill(page.bytes_.begin(), page.bytes_.end(), std::byte{0U});
    const std::span<std::byte> bytes = page.mutable_bytes();
    WriteUInt32At(bytes, kMagicOffset, kPageMagic);
    WriteUInt32At(bytes, kVersionOffset, kPersistentFormatVersion);
    WriteUInt16At(bytes, kTypeOffset, static_cast<std::uint16_t>(type));
    WriteUInt16At(bytes, kHeaderSizeOffset, static_cast<std::uint16_t>(kPageHeaderSize));
    WriteUInt64At(bytes, kPageIdOffset, page_id.value());
    WriteUInt64At(bytes, kPageLsnOffset, 0U);
    WriteUInt32At(bytes, kChecksumOffset, 0U);
    WriteUInt32At(bytes, kFlagsOffset, 0U);
    WriteUInt32At(bytes, kReservedOffset, 0U);
    const Status status = page.Finalize();
    if (!status.ok()) {
        return status;
    }
    return page;
}

Result<Page> Page::Deserialize(
    std::span<const std::byte> bytes,
    std::optional<PageId> expected_page_id) {
    if (bytes.size() != kInitialPageSizeBytes) {
        return Corruption("persistent page does not have the configured page size")
            .WithContext("actual_size", std::to_string(bytes.size()));
    }

    Page page;
    std::copy(bytes.begin(), bytes.end(), page.bytes_.begin());
    const std::span<const std::byte> page_bytes = page.bytes();
    if (ReadUInt32At(page_bytes, kMagicOffset) != kPageMagic) {
        return Corruption("page magic does not match KernDB format");
    }
    if (ReadUInt32At(page_bytes, kVersionOffset) != kPersistentFormatVersion) {
        return Status::Error(ErrorCode::kUnsupported, "page format version is unsupported")
            .WithContext("version", std::to_string(ReadUInt32At(page_bytes, kVersionOffset)));
    }
    if (ReadUInt16At(page_bytes, kHeaderSizeOffset) != kPageHeaderSize) {
        return Corruption("page header size is invalid");
    }
    const auto page_type = DecodePageType(ReadUInt16At(page_bytes, kTypeOffset));
    if (!page_type.ok()) {
        return page_type.status();
    }
    const PageId page_id{ReadUInt64At(page_bytes, kPageIdOffset)};
    if (!page_id.valid()) {
        return Corruption("page contains an invalid page ID");
    }
    if (expected_page_id.has_value() && page_id != expected_page_id.value()) {
        return Corruption("page ID does not match its physical page location")
            .WithContext("expected_page_id", ToString(expected_page_id.value()))
            .WithContext("actual_page_id", ToString(page_id));
    }
    const std::uint32_t expected_checksum = ReadUInt32At(page_bytes, kChecksumOffset);
    if (expected_checksum != Checksum(page_bytes)) {
        return Corruption("page checksum does not match page contents")
            .WithContext("page_id", ToString(page_id));
    }
    return page;
}

PageId Page::page_id() const noexcept {
    return PageId{ReadUInt64At(bytes(), kPageIdOffset)};
}

PageType Page::type() const noexcept {
    const auto type = DecodePageType(ReadUInt16At(bytes(), kTypeOffset));
    return type.ok() ? type.value() : PageType::kHeap;
}

std::uint64_t Page::page_lsn() const noexcept {
    return ReadUInt64At(bytes(), kPageLsnOffset);
}

std::span<const std::byte> Page::bytes() const noexcept {
    return bytes_;
}

std::span<std::byte> Page::mutable_bytes() noexcept {
    return bytes_;
}

std::span<const std::byte> Page::payload() const noexcept {
    return bytes().subspan(kPageHeaderSize);
}

std::span<std::byte> Page::mutable_payload() noexcept {
    return mutable_bytes().subspan(kPageHeaderSize);
}

Status Page::Finalize() {
    const std::span<std::byte> page_bytes = mutable_bytes();
    WriteUInt32At(page_bytes, kChecksumOffset, 0U);
    WriteUInt32At(page_bytes, kChecksumOffset, Checksum(page_bytes));
    return Status::Ok();
}

Result<PageType> Page::DecodePageType(std::uint16_t raw_type) {
    switch (static_cast<PageType>(raw_type)) {
        case PageType::kCatalog:
        case PageType::kHeap:
            return static_cast<PageType>(raw_type);
    }
    return Corruption("page type is invalid").WithContext("page_type", std::to_string(raw_type));
}

std::uint32_t Page::Checksum(std::span<const std::byte> bytes) noexcept {
    std::uint32_t checksum = 2166136261U;
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const std::uint8_t value =
            (index >= kChecksumOffset && index < kChecksumOffset + sizeof(std::uint32_t))
                ? 0U
                : std::to_integer<std::uint8_t>(bytes[index]);
        checksum ^= value;
        checksum *= 16777619U;
    }
    return checksum;
}

}  // namespace kerndb::storage
