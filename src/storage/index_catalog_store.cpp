#include "storage/index_catalog_store.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include "kerndb/byte_codec.h"
#include "kerndb/storage/heap.h"

namespace kerndb::storage {
namespace {

constexpr std::uint16_t kIndexCatalogFormatVersion = 1U;

[[nodiscard]] Result<std::vector<std::byte>> EncodeIndexCatalogRecord(const InMemoryIndex& index) {
    if (!index.id.valid() || index.id.value() == 0U || index.name.empty() ||
        !index.table_id.valid() || !index.root_page_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "index catalog entry is incomplete");
    }
    if (index.name.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Status::Error(ErrorCode::kResourceExhausted, "index catalog name is too long");
    }
    ByteWriter writer;
    writer.WriteUInt16(kIndexCatalogFormatVersion);
    writer.WriteUInt64(index.id.value());
    writer.WriteUInt64(index.table_id.value());
    writer.WriteUInt64(static_cast<std::uint64_t>(index.column_index));
    writer.WriteUInt64(index.root_page_id.value());
    writer.WriteUInt16(static_cast<std::uint16_t>(index.name.size()));
    writer.WriteBytes(std::as_bytes(std::span<const char>(index.name.data(), index.name.size())));
    return std::move(writer).TakeBytes();
}

[[nodiscard]] Result<InMemoryIndex> DecodeIndexCatalogRecord(std::span<const std::byte> bytes) {
    ByteReader reader(bytes);
    const auto version = reader.ReadUInt16();
    const auto index_id = reader.ReadUInt64();
    const auto table_id = reader.ReadUInt64();
    const auto column_index = reader.ReadUInt64();
    const auto root_page_id = reader.ReadUInt64();
    const auto name_length = reader.ReadUInt16();
    if (!version.ok() || !index_id.ok() || !table_id.ok() || !column_index.ok() ||
        !root_page_id.ok() || !name_length.ok()) {
        return Status::Error(ErrorCode::kCorruption, "index catalog record header is malformed");
    }
    if (version.value() != kIndexCatalogFormatVersion || index_id.value() == 0U ||
        index_id.value() == IndexId::kInvalidValue || table_id.value() == TableId::kInvalidValue ||
        root_page_id.value() == PageId::kInvalidValue ||
        column_index.value() > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::Error(ErrorCode::kCorruption, "index catalog record contains invalid metadata");
    }
    const auto name = reader.ReadBytes(name_length.value());
    if (!name.ok() || name.value().empty() || reader.remaining() != 0U) {
        return Status::Error(ErrorCode::kCorruption, "index catalog record is malformed");
    }
    return InMemoryIndex{
        .id = IndexId{index_id.value()},
        .name = std::string(reinterpret_cast<const char*>(name.value().data()), name.value().size()),
        .table_id = TableId{table_id.value()},
        .column_index = static_cast<std::size_t>(column_index.value()),
        .root_page_id = PageId{root_page_id.value()},
    };
}

}  // namespace

Result<IndexCatalogStore> IndexCatalogStore::Open(const std::filesystem::path& path) {
    auto page_manager = PageManager::Open(path, true);
    if (!page_manager.ok()) {
        return page_manager.status();
    }
    return IndexCatalogStore{std::move(page_manager).value()};
}

Result<std::vector<InMemoryIndex>> IndexCatalogStore::Load() const {
    const auto page_count = page_manager_.PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    std::map<std::uint64_t, InMemoryIndex> latest_by_id;
    std::map<std::string, std::uint64_t, std::less<>> ids_by_name;
    for (std::uint64_t raw_page_id = 0U; raw_page_id < page_count.value(); ++raw_page_id) {
        const auto page = page_manager_.ReadPage(PageId{raw_page_id});
        if (!page.ok()) {
            return page.status();
        }
        if (page.value().type() != PageType::kCatalog) {
            return Status::Error(ErrorCode::kCorruption, "index catalog contains a non-catalog page");
        }
        const auto slotted_page = SlottedPage::Load(page.value());
        if (!slotted_page.ok()) {
            return slotted_page.status();
        }
        const auto records = slotted_page.value().Records();
        if (!records.ok()) {
            return records.status();
        }
        for (const auto& [slot_id, bytes] : records.value()) {
            static_cast<void>(slot_id);
            const auto decoded = DecodeIndexCatalogRecord(bytes);
            if (!decoded.ok()) {
                return decoded.status();
            }
            const InMemoryIndex& candidate = decoded.value();
            const auto existing = latest_by_id.find(candidate.id.value());
            if (existing != latest_by_id.end()) {
                if (existing->second.name != candidate.name || existing->second.table_id != candidate.table_id ||
                    existing->second.column_index != candidate.column_index) {
                    return Status::Error(ErrorCode::kCorruption, "index catalog history changes immutable metadata");
                }
            } else {
                const auto [name_iterator, inserted] = ids_by_name.emplace(candidate.name, candidate.id.value());
                if (!inserted && name_iterator->second != candidate.id.value()) {
                    return Status::Error(ErrorCode::kCorruption, "index catalog contains duplicate names");
                }
            }
            latest_by_id[candidate.id.value()] = candidate;
        }
    }

    std::vector<InMemoryIndex> indexes;
    indexes.reserve(latest_by_id.size());
    for (const auto& [unused_id, index] : latest_by_id) {
        static_cast<void>(unused_id);
        indexes.push_back(index);
    }
    return indexes;
}

Status IndexCatalogStore::Append(const InMemoryIndex& index) {
    const auto record = EncodeIndexCatalogRecord(index);
    if (!record.ok()) {
        return record.status();
    }
    const auto page_count = page_manager_.PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    for (std::uint64_t raw_page_id = 0U; raw_page_id < page_count.value(); ++raw_page_id) {
        const auto page = page_manager_.ReadPage(PageId{raw_page_id});
        if (!page.ok()) {
            return page.status();
        }
        if (page.value().type() != PageType::kCatalog) {
            return Status::Error(ErrorCode::kCorruption, "index catalog contains a non-catalog page");
        }
        const auto loaded = SlottedPage::Load(page.value());
        if (!loaded.ok()) {
            return loaded.status();
        }
        auto mutable_page = std::move(loaded).value();
        const auto inserted = mutable_page.Insert(record.value());
        if (!inserted.ok()) {
            return inserted.status();
        }
        if (inserted.value().has_value()) {
            return page_manager_.WritePage(mutable_page.page());
        }
    }

    const auto page = page_manager_.AllocatePage(PageType::kCatalog);
    if (!page.ok()) {
        return page.status();
    }
    const auto initialized = SlottedPage::Initialize(page.value());
    if (!initialized.ok()) {
        return initialized.status();
    }
    auto mutable_page = std::move(initialized).value();
    const auto inserted = mutable_page.Insert(record.value());
    if (!inserted.ok()) {
        return inserted.status();
    }
    if (!inserted.value().has_value()) {
        return Status::Error(ErrorCode::kResourceExhausted, "index catalog record cannot fit on an empty page");
    }
    return page_manager_.WritePage(mutable_page.page());
}

IndexCatalogStore::IndexCatalogStore(PageManager page_manager)
    : page_manager_(std::move(page_manager)) {}

}  // namespace kerndb::storage
