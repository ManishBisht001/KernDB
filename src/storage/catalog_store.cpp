#include "storage/catalog_store.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "kerndb/byte_codec.h"
#include "kerndb/storage/heap.h"

namespace kerndb::storage {
namespace {

[[nodiscard]] Result<std::vector<std::byte>> EncodeCatalogRecord(const InMemoryTable& table) {
    if (!table.id.valid() || table.name.empty() || table.schema.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "catalog entry is incomplete");
    }
    if (table.name.size() > std::numeric_limits<std::uint16_t>::max() ||
        table.schema.size() > std::numeric_limits<std::uint16_t>::max()) {
        return Status::Error(ErrorCode::kResourceExhausted, "catalog entry exceeds Phase 2 limits");
    }

    ByteWriter writer;
    writer.WriteUInt64(table.id.value());
    writer.WriteUInt16(static_cast<std::uint16_t>(table.name.size()));
    writer.WriteBytes(std::as_bytes(std::span<const char>(table.name.data(), table.name.size())));
    writer.WriteUInt16(static_cast<std::uint16_t>(table.schema.size()));
    for (const ColumnDefinition& column : table.schema) {
        if (column.name.empty() || column.name.size() > std::numeric_limits<std::uint16_t>::max()) {
            return Status::Error(ErrorCode::kInvalidArgument, "catalog column name is invalid");
        }
        writer.WriteUInt16(static_cast<std::uint16_t>(column.name.size()));
        writer.WriteBytes(std::as_bytes(std::span<const char>(column.name.data(), column.name.size())));
        writer.WriteUInt8(static_cast<std::uint8_t>(column.type));
    }
    return std::move(writer).TakeBytes();
}

[[nodiscard]] Result<InMemoryTable> DecodeCatalogRecord(std::span<const std::byte> bytes) {
    ByteReader reader(bytes);
    const auto table_id = reader.ReadUInt64();
    const auto name_length = reader.ReadUInt16();
    if (!table_id.ok() || !name_length.ok()) {
        return Status::Error(ErrorCode::kCorruption, "catalog record header is malformed");
    }
    if (table_id.value() == 0U || table_id.value() == TableId::kInvalidValue) {
        return Status::Error(ErrorCode::kCorruption, "catalog record table ID is invalid");
    }
    const auto name_bytes = reader.ReadBytes(name_length.value());
    const auto column_count = reader.ReadUInt16();
    if (!name_bytes.ok() || !column_count.ok() || name_bytes.value().empty() || column_count.value() == 0U) {
        return Status::Error(ErrorCode::kCorruption, "catalog record table metadata is malformed");
    }

    Schema schema;
    schema.reserve(column_count.value());
    for (std::uint16_t index = 0U; index < column_count.value(); ++index) {
        const auto column_name_length = reader.ReadUInt16();
        if (!column_name_length.ok() || column_name_length.value() == 0U) {
            return Status::Error(ErrorCode::kCorruption, "catalog record column name is malformed");
        }
        const auto column_name = reader.ReadBytes(column_name_length.value());
        const auto raw_type = reader.ReadUInt8();
        if (!column_name.ok() || !raw_type.ok()) {
            return Status::Error(ErrorCode::kCorruption, "catalog record column is truncated");
        }
        ColumnType type = ColumnType::kInt;
        if (raw_type.value() == static_cast<std::uint8_t>(ColumnType::kInt)) {
            type = ColumnType::kInt;
        } else if (raw_type.value() == static_cast<std::uint8_t>(ColumnType::kText)) {
            type = ColumnType::kText;
        } else {
            return Status::Error(ErrorCode::kCorruption, "catalog record column type is invalid");
        }
        schema.push_back(ColumnDefinition{
            .name = std::string(
                reinterpret_cast<const char*>(column_name.value().data()),
                column_name.value().size()),
            .type = type,
        });
    }
    if (reader.remaining() != 0U) {
        return Status::Error(ErrorCode::kCorruption, "catalog record has trailing bytes");
    }
    return InMemoryTable{
        .id = TableId{table_id.value()},
        .name = std::string(
            reinterpret_cast<const char*>(name_bytes.value().data()),
            name_bytes.value().size()),
        .schema = std::move(schema),
        .tuples = {},
    };
}

}  // namespace

Result<CatalogStore> CatalogStore::Open(const std::filesystem::path& path) {
    auto manager = PageManager::Open(path, true);
    if (!manager.ok()) {
        return manager.status();
    }
    return CatalogStore{std::move(manager).value()};
}

Result<std::vector<InMemoryTable>> CatalogStore::Load() const {
    const auto page_count = page_manager_.PageCount();
    if (!page_count.ok()) {
        return page_count.status();
    }
    std::vector<InMemoryTable> tables;
    for (std::uint64_t raw_page_id = 0U; raw_page_id < page_count.value(); ++raw_page_id) {
        const auto page = page_manager_.ReadPage(PageId{raw_page_id});
        if (!page.ok()) {
            return page.status();
        }
        if (page.value().type() != PageType::kCatalog) {
            return Status::Error(ErrorCode::kCorruption, "catalog file contains a non-catalog page");
        }
        const auto slotted_page = SlottedPage::Load(page.value());
        if (!slotted_page.ok()) {
            return slotted_page.status();
        }
        const auto records = slotted_page.value().Records();
        if (!records.ok()) {
            return records.status();
        }
        for (const auto& [slot_id, record] : records.value()) {
            static_cast<void>(slot_id);
            const auto table = DecodeCatalogRecord(record);
            if (!table.ok()) {
                return table.status();
            }
            tables.push_back(table.value());
        }
    }
    return tables;
}

Status CatalogStore::Append(const InMemoryTable& table) {
    const auto record = EncodeCatalogRecord(table);
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
            return Status::Error(ErrorCode::kCorruption, "catalog file contains a non-catalog page");
        }
        const auto loaded = SlottedPage::Load(page.value());
        if (!loaded.ok()) {
            return loaded.status();
        }
        auto mutable_page = std::move(loaded).value();
        const auto slot = mutable_page.Insert(record.value());
        if (!slot.ok()) {
            return slot.status();
        }
        if (slot.value().has_value()) {
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
    const auto slot = mutable_page.Insert(record.value());
    if (!slot.ok()) {
        return slot.status();
    }
    if (!slot.value().has_value()) {
        return Status::Error(ErrorCode::kResourceExhausted, "catalog record cannot fit on an empty page");
    }
    return page_manager_.WritePage(mutable_page.page());
}

CatalogStore::CatalogStore(PageManager page_manager)
    : page_manager_(std::move(page_manager)) {}

}  // namespace kerndb::storage
