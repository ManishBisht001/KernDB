#include "kerndb/storage/heap.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "kerndb/storage/record.h"

namespace kerndb::storage {
namespace {

constexpr std::size_t kSlotCountOffset = kPageHeaderSize;
constexpr std::size_t kFreeStartOffset = kPageHeaderSize + 2U;
constexpr std::size_t kFreeEndOffset = kPageHeaderSize + 4U;
constexpr std::size_t kHeapHeaderSize = 8U;
constexpr std::size_t kRecordDataStart = kPageHeaderSize + kHeapHeaderSize;
constexpr std::size_t kSlotSize = 12U;
constexpr std::uint32_t kSlotAllocated = 1U;

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

[[nodiscard]] Status HeapCorruption(std::string message) {
    return Status::Error(ErrorCode::kCorruption, std::move(message));
}

[[nodiscard]] std::size_t SlotOffset(SlotId slot_id) {
    return kInitialPageSizeBytes - (static_cast<std::size_t>(slot_id.value()) + 1U) * kSlotSize;
}

}  // namespace

Result<SlottedPage> SlottedPage::Initialize(Page page) {
    if (page.type() != PageType::kHeap && page.type() != PageType::kCatalog) {
        return Status::Error(ErrorCode::kInvalidArgument, "slotted page type must be catalog or heap");
    }
    std::span<std::byte> bytes = page.mutable_bytes();
    WriteUInt16At(bytes, kSlotCountOffset, 0U);
    WriteUInt16At(bytes, kFreeStartOffset, static_cast<std::uint16_t>(kRecordDataStart));
    WriteUInt16At(bytes, kFreeEndOffset, static_cast<std::uint16_t>(kInitialPageSizeBytes));
    WriteUInt16At(bytes, kPageHeaderSize + 6U, 0U);
    const Status status = page.Finalize();
    if (!status.ok()) {
        return status;
    }
    return SlottedPage{std::move(page)};
}

Result<SlottedPage> SlottedPage::Load(Page page) {
    SlottedPage slotted_page{std::move(page)};
    const Status status = slotted_page.Validate();
    if (!status.ok()) {
        return status;
    }
    return slotted_page;
}

Result<std::optional<SlotId>> SlottedPage::Insert(std::span<const std::byte> record) {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    if (record.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "cannot insert an empty record");
    }
    if (record.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Status::Error(ErrorCode::kResourceExhausted, "record is too large for page offsets");
    }
    if (record.size() + kSlotSize > FreeBytes()) {
        return std::optional<SlotId>{};
    }

    const auto slot_count = SlotCount();
    const auto free_start = FreeStart();
    const auto free_end = FreeEnd();
    if (!slot_count.ok() || !free_start.ok() || !free_end.ok()) {
        return Status::Error(ErrorCode::kInternal, "validated heap page could not read header");
    }
    if (slot_count.value() == SlotId::kInvalidValue) {
        return Status::Error(ErrorCode::kResourceExhausted, "slot ID space is exhausted");
    }

    const SlotId slot_id{slot_count.value()};
    const std::size_t next_free_start = free_start.value() + record.size();
    const std::size_t next_free_end = free_end.value() - kSlotSize;
    std::span<std::byte> bytes = page_.mutable_bytes();
    for (std::size_t index = 0U; index < record.size(); ++index) {
        bytes[static_cast<std::size_t>(free_start.value()) + index] = record[index];
    }
    WriteUInt16At(bytes, SlotOffset(slot_id), free_start.value());
    WriteUInt16At(
        bytes,
        SlotOffset(slot_id) + 2U,
        static_cast<std::uint16_t>(record.size()));
    WriteUInt32At(bytes, SlotOffset(slot_id) + 4U, 1U);
    WriteUInt32At(bytes, SlotOffset(slot_id) + 8U, kSlotAllocated);
    WriteUInt16At(bytes, kSlotCountOffset, static_cast<std::uint16_t>(slot_count.value() + 1U));
    WriteUInt16At(bytes, kFreeStartOffset, static_cast<std::uint16_t>(next_free_start));
    WriteUInt16At(bytes, kFreeEndOffset, static_cast<std::uint16_t>(next_free_end));
    const Status finalize_status = page_.Finalize();
    if (!finalize_status.ok()) {
        return finalize_status;
    }
    return std::optional<SlotId>{slot_id};
}

Result<std::vector<std::byte>> SlottedPage::Read(SlotId slot_id) const {
    const auto bounds = SlotBounds(slot_id);
    if (!bounds.ok()) {
        return bounds.status();
    }
    const std::span<const std::byte> bytes = page_.bytes();
    return std::vector<std::byte>(
        bytes.begin() + bounds.value().first,
        bytes.begin() + bounds.value().first + bounds.value().second);
}

Result<std::vector<std::pair<SlotId, std::vector<std::byte>>>> SlottedPage::Records() const {
    const auto slot_count = SlotCount();
    if (!slot_count.ok()) {
        return slot_count.status();
    }
    std::vector<std::pair<SlotId, std::vector<std::byte>>> records;
    records.reserve(slot_count.value());
    for (std::uint32_t raw_slot = 0U; raw_slot < slot_count.value(); ++raw_slot) {
        const SlotId slot_id{raw_slot};
        const auto record = Read(slot_id);
        if (!record.ok()) {
            return record.status();
        }
        records.emplace_back(slot_id, record.value());
    }
    return records;
}

std::size_t SlottedPage::FreeBytes() const {
    const auto free_start = FreeStart();
    const auto free_end = FreeEnd();
    if (!free_start.ok() || !free_end.ok() || free_end.value() < free_start.value()) {
        return 0U;
    }
    return free_end.value() - free_start.value();
}

const Page& SlottedPage::page() const noexcept {
    return page_;
}

SlottedPage::SlottedPage(Page page)
    : page_(std::move(page)) {}

Result<std::uint16_t> SlottedPage::SlotCount() const {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    return ReadUInt16At(page_.bytes(), kSlotCountOffset);
}

Result<std::uint16_t> SlottedPage::FreeStart() const {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    return ReadUInt16At(page_.bytes(), kFreeStartOffset);
}

Result<std::uint16_t> SlottedPage::FreeEnd() const {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    return ReadUInt16At(page_.bytes(), kFreeEndOffset);
}

Result<std::pair<std::uint16_t, std::uint16_t>> SlottedPage::SlotBounds(SlotId slot_id) const {
    const Status status = Validate();
    if (!status.ok()) {
        return status;
    }
    const auto slot_count = SlotCount();
    if (!slot_count.ok()) {
        return slot_count.status();
    }
    if (!slot_id.valid() || slot_id.value() >= slot_count.value()) {
        return Status::Error(ErrorCode::kOutOfRange, "slot ID does not exist on page")
            .WithContext("slot_id", ToString(slot_id));
    }
    const std::span<const std::byte> bytes = page_.bytes();
    const std::size_t offset = SlotOffset(slot_id);
    if (ReadUInt32At(bytes, offset + 8U) != kSlotAllocated) {
        return Status::Error(ErrorCode::kNotFound, "slot is not allocated")
            .WithContext("slot_id", ToString(slot_id));
    }
    return std::pair{
        ReadUInt16At(bytes, offset),
        ReadUInt16At(bytes, offset + 2U),
    };
}

Status SlottedPage::Validate() const {
    if (page_.type() != PageType::kHeap && page_.type() != PageType::kCatalog) {
        return HeapCorruption("slotted page has an invalid page type");
    }
    const std::span<const std::byte> bytes = page_.bytes();
    const std::uint16_t slot_count = ReadUInt16At(bytes, kSlotCountOffset);
    const std::uint16_t free_start = ReadUInt16At(bytes, kFreeStartOffset);
    const std::uint16_t free_end = ReadUInt16At(bytes, kFreeEndOffset);
    if (free_start < kRecordDataStart || free_start > free_end || free_end > kInitialPageSizeBytes) {
        return HeapCorruption("slotted page free-space bounds are invalid");
    }
    if (static_cast<std::size_t>(slot_count) * kSlotSize >
        kInitialPageSizeBytes - free_end) {
        return HeapCorruption("slotted page slot directory overlaps free space");
    }
    for (std::uint32_t raw_slot = 0U; raw_slot < slot_count; ++raw_slot) {
        const std::size_t offset = SlotOffset(SlotId{raw_slot});
        const std::uint16_t tuple_offset = ReadUInt16At(bytes, offset);
        const std::uint16_t tuple_length = ReadUInt16At(bytes, offset + 2U);
        const std::uint32_t flags = ReadUInt32At(bytes, offset + 8U);
        if (flags != kSlotAllocated ||
            tuple_offset < kRecordDataStart ||
            tuple_offset > free_start ||
            tuple_length > free_start - tuple_offset) {
            return HeapCorruption("slotted page contains an invalid slot entry")
                .WithContext("slot_id", std::to_string(raw_slot));
        }
    }
    return Status::Ok();
}

TableHeap::TableHeap(PageManager& page_manager)
    : page_manager_(page_manager) {}

Result<RecordId> TableHeap::Insert(const Schema& schema, const Tuple& tuple) {
    const auto record = EncodeRecord(schema, tuple);
    if (!record.ok()) {
        return record.status();
    }
    const auto page_count = page_manager_.PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    for (std::uint64_t raw_page_id = 0U; raw_page_id < page_count.value(); ++raw_page_id) {
        const PageId page_id{raw_page_id};
        const auto page = page_manager_.ReadPage(page_id);
        if (!page.ok()) {
            return page.status();
        }
        if (page.value().type() != PageType::kHeap) {
            return HeapCorruption("table file contains a non-heap page")
                .WithContext("page_id", ToString(page_id));
        }
        const auto heap_page = SlottedPage::Load(page.value());
        if (!heap_page.ok()) {
            return heap_page.status();
        }
        auto mutable_heap_page = std::move(heap_page).value();
        const auto slot = mutable_heap_page.Insert(record.value());
        if (!slot.ok()) {
            return slot.status();
        }
        if (!slot.value().has_value()) {
            continue;
        }
        const Status write_status = page_manager_.WritePage(mutable_heap_page.page());
        if (!write_status.ok()) {
            return write_status;
        }
        return RecordId{
            .page_id = page_id,
            .slot_id = slot.value().value(),
        };
    }

    const auto new_page = page_manager_.AllocatePage(PageType::kHeap);
    if (!new_page.ok()) {
        return new_page.status();
    }
    const auto heap_page = SlottedPage::Initialize(new_page.value());
    if (!heap_page.ok()) {
        return heap_page.status();
    }
    auto mutable_heap_page = std::move(heap_page).value();
    const auto slot = mutable_heap_page.Insert(record.value());
    if (!slot.ok()) {
        return slot.status();
    }
    if (!slot.value().has_value()) {
        return Status::Error(ErrorCode::kResourceExhausted, "record cannot fit on an empty heap page");
    }
    const Status write_status = page_manager_.WritePage(mutable_heap_page.page());
    if (!write_status.ok()) {
        return write_status;
    }
    return RecordId{
        .page_id = new_page.value().page_id(),
        .slot_id = slot.value().value(),
    };
}

Result<Tuple> TableHeap::Read(const Schema& schema, RecordId record_id) const {
    if (!record_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "record ID is invalid");
    }
    const auto page = page_manager_.ReadPage(record_id.page_id);
    if (!page.ok()) {
        return page.status();
    }
    if (page.value().type() != PageType::kHeap) {
        return HeapCorruption("record page is not a heap page");
    }
    const auto heap_page = SlottedPage::Load(page.value());
    if (!heap_page.ok()) {
        return heap_page.status();
    }
    const auto record = heap_page.value().Read(record_id.slot_id);
    if (!record.ok()) {
        return record.status();
    }
    return DecodeRecord(schema, record.value());
}

Result<std::vector<Tuple>> TableHeap::Scan(const Schema& schema) const {
    const auto page_count = page_manager_.PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    std::vector<Tuple> tuples;
    for (std::uint64_t raw_page_id = 0U; raw_page_id < page_count.value(); ++raw_page_id) {
        const auto page = page_manager_.ReadPage(PageId{raw_page_id});
        if (!page.ok()) {
            return page.status();
        }
        if (page.value().type() != PageType::kHeap) {
            return HeapCorruption("table file contains a non-heap page")
                .WithContext("page_id", std::to_string(raw_page_id));
        }
        const auto heap_page = SlottedPage::Load(page.value());
        if (!heap_page.ok()) {
            return heap_page.status();
        }
        const auto records = heap_page.value().Records();
        if (!records.ok()) {
            return records.status();
        }
        for (const auto& [slot_id, bytes] : records.value()) {
            static_cast<void>(slot_id);
            const auto tuple = DecodeRecord(schema, bytes);
            if (!tuple.ok()) {
                return tuple.status();
            }
            tuples.push_back(tuple.value());
        }
    }
    return tuples;
}

}  // namespace kerndb::storage
