#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "kerndb/ids.h"
#include "kerndb/result.h"
#include "kerndb/storage/page_manager.h"
#include "kerndb/types.h"

namespace kerndb::storage {

class SlottedPage {
public:
    [[nodiscard]] static Result<SlottedPage> Initialize(Page page);
    [[nodiscard]] static Result<SlottedPage> Load(Page page);

    [[nodiscard]] Result<std::optional<SlotId>> Insert(std::span<const std::byte> record);
    [[nodiscard]] Result<std::vector<std::byte>> Read(SlotId slot_id) const;
    [[nodiscard]] Result<std::vector<std::pair<SlotId, std::vector<std::byte>>>> Records() const;
    [[nodiscard]] std::size_t FreeBytes() const;
    [[nodiscard]] const Page& page() const noexcept;

private:
    explicit SlottedPage(Page page);

    [[nodiscard]] Result<std::uint16_t> SlotCount() const;
    [[nodiscard]] Result<std::uint16_t> FreeStart() const;
    [[nodiscard]] Result<std::uint16_t> FreeEnd() const;
    [[nodiscard]] Result<std::pair<std::uint16_t, std::uint16_t>> SlotBounds(SlotId slot_id) const;
    [[nodiscard]] Status Validate() const;

    Page page_;
};

class TableHeap {
public:
    explicit TableHeap(PageManager& page_manager);

    [[nodiscard]] Result<RecordId> Insert(const Schema& schema, const Tuple& tuple);
    [[nodiscard]] Result<Tuple> Read(const Schema& schema, RecordId record_id) const;
    [[nodiscard]] Result<std::vector<Tuple>> Scan(const Schema& schema) const;
    [[nodiscard]] Result<std::vector<std::pair<RecordId, Tuple>>> ScanWithRecordIds(
        const Schema& schema) const;

private:
    PageManager& page_manager_;
};

}  // namespace kerndb::storage
