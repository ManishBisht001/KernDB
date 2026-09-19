#include "catalog/catalog.h"

#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace kerndb {

std::string NormalizeIdentifier(std::string_view identifier) {
    std::string normalized;
    normalized.reserve(identifier.size());
    for (const char character : identifier) {
        normalized.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return normalized;
}

Result<TableId> InMemoryCatalog::CreateTable(std::string name, Schema schema) {
    name = NormalizeIdentifier(name);
    if (name.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "table name cannot be empty");
    }
    if (schema.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "a table requires at least one column");
    }
    if (table_ids_by_name_.contains(name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "table already exists")
            .WithContext("table", name);
    }
    if (next_table_id_ == TableId::kInvalidValue) {
        return Status::Error(ErrorCode::kResourceExhausted, "table ID space is exhausted");
    }

    const TableId table_id = NextTableId();
    const auto [table_iterator, inserted] = tables_by_id_.emplace(
        table_id.value(),
        InMemoryTable{
            .id = table_id,
            .name = name,
            .schema = std::move(schema),
            .tuples = {},
        });
    if (!inserted) {
        return Status::Error(ErrorCode::kInternal, "failed to register a newly created table");
    }
    table_ids_by_name_.emplace(table_iterator->second.name, table_id.value());
    return table_id;
}

Status InMemoryCatalog::LoadTable(TableId id, std::string name, Schema schema) {
    name = NormalizeIdentifier(name);
    if (!id.valid() || id.value() == 0U) {
        return Status::Error(ErrorCode::kCorruption, "catalog contains an invalid table ID");
    }
    if (name.empty() || schema.empty()) {
        return Status::Error(ErrorCode::kCorruption, "catalog contains incomplete table metadata");
    }
    if (table_ids_by_name_.contains(name) || tables_by_id_.contains(id.value())) {
        return Status::Error(ErrorCode::kCorruption, "catalog contains duplicate table metadata")
            .WithContext("table", name);
    }
    tables_by_id_.emplace(
        id.value(),
        InMemoryTable{
            .id = id,
            .name = std::move(name),
            .schema = std::move(schema),
            .tuples = {},
        });
    table_ids_by_name_.emplace(tables_by_id_.at(id.value()).name, id.value());
    if (id.value() >= next_table_id_) {
        if (id.value() == TableId::kInvalidValue - 1U) {
            next_table_id_ = TableId::kInvalidValue;
        } else {
            next_table_id_ = id.value() + 1U;
        }
    }
    return Status::Ok();
}

Status InMemoryCatalog::RemoveTable(TableId id) {
    const auto iterator = tables_by_id_.find(id.value());
    if (iterator == tables_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "table ID does not exist")
            .WithContext("table_id", ToString(id));
    }
    table_ids_by_name_.erase(iterator->second.name);
    tables_by_id_.erase(iterator);
    return Status::Ok();
}

Result<InMemoryTable*> InMemoryCatalog::FindTableByName(std::string_view name) {
    const std::string normalized = NormalizeIdentifier(name);
    const auto name_iterator = table_ids_by_name_.find(normalized);
    if (name_iterator == table_ids_by_name_.end()) {
        return Status::Error(ErrorCode::kNotFound, "table does not exist")
            .WithContext("table", normalized);
    }
    return FindTableById(TableId{name_iterator->second});
}

Result<const InMemoryTable*> InMemoryCatalog::FindTableByName(std::string_view name) const {
    const std::string normalized = NormalizeIdentifier(name);
    const auto name_iterator = table_ids_by_name_.find(normalized);
    if (name_iterator == table_ids_by_name_.end()) {
        return Status::Error(ErrorCode::kNotFound, "table does not exist")
            .WithContext("table", normalized);
    }
    return FindTableById(TableId{name_iterator->second});
}

Result<InMemoryTable*> InMemoryCatalog::FindTableById(TableId id) {
    const auto iterator = tables_by_id_.find(id.value());
    if (iterator == tables_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "table ID does not exist")
            .WithContext("table_id", ToString(id));
    }
    return &iterator->second;
}

Result<const InMemoryTable*> InMemoryCatalog::FindTableById(TableId id) const {
    const auto iterator = tables_by_id_.find(id.value());
    if (iterator == tables_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "table ID does not exist")
            .WithContext("table_id", ToString(id));
    }
    return &iterator->second;
}

Result<IndexId> InMemoryCatalog::CreateIndex(
    std::string name,
    TableId table_id,
    std::size_t column_index) {
    name = NormalizeIdentifier(name);
    if (name.empty()) {
        return Status::Error(ErrorCode::kInvalidArgument, "index name cannot be empty");
    }
    const auto table = FindTableById(table_id);
    if (!table.ok()) {
        return table.status();
    }
    if (column_index >= table.value()->schema.size()) {
        return Status::Error(ErrorCode::kOutOfRange, "index column is outside the table schema");
    }
    if (index_ids_by_name_.contains(name)) {
        return Status::Error(ErrorCode::kAlreadyExists, "index already exists")
            .WithContext("index", name);
    }
    for (const auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id == table_id && index.column_index == column_index) {
            return Status::Error(ErrorCode::kAlreadyExists, "an index already exists for this column")
                .WithContext("table_id", ToString(table_id));
        }
    }
    if (next_index_id_ == IndexId::kInvalidValue) {
        return Status::Error(ErrorCode::kResourceExhausted, "index ID space is exhausted");
    }

    const IndexId index_id = NextIndexId();
    const auto [iterator, inserted] = indexes_by_id_.emplace(
        index_id.value(),
        InMemoryIndex{
            .id = index_id,
            .name = name,
            .table_id = table_id,
            .column_index = column_index,
            .root_page_id = PageId{},
        });
    if (!inserted) {
        return Status::Error(ErrorCode::kInternal, "failed to register a newly created index");
    }
    index_ids_by_name_.emplace(iterator->second.name, index_id.value());
    return index_id;
}

Status InMemoryCatalog::LoadIndex(InMemoryIndex index) {
    index.name = NormalizeIdentifier(index.name);
    if (!index.id.valid() || index.id.value() == 0U || index.name.empty() ||
        !index.table_id.valid() || !index.root_page_id.valid()) {
        return Status::Error(ErrorCode::kCorruption, "catalog contains incomplete index metadata");
    }
    const auto table = FindTableById(index.table_id);
    if (!table.ok() || index.column_index >= table.value()->schema.size()) {
        return Status::Error(ErrorCode::kCorruption, "catalog index references an invalid table column");
    }
    if (index_ids_by_name_.contains(index.name) || indexes_by_id_.contains(index.id.value())) {
        return Status::Error(ErrorCode::kCorruption, "catalog contains duplicate index metadata")
            .WithContext("index", index.name);
    }
    for (const auto& [unused_id, existing] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (existing.table_id == index.table_id && existing.column_index == index.column_index) {
            return Status::Error(ErrorCode::kCorruption, "catalog contains duplicate column indexes");
        }
    }
    const std::uint64_t loaded_id = index.id.value();
    index_ids_by_name_.emplace(index.name, loaded_id);
    indexes_by_id_.emplace(loaded_id, std::move(index));
    if (loaded_id >= next_index_id_) {
        next_index_id_ = loaded_id == IndexId::kInvalidValue - 1U
                             ? IndexId::kInvalidValue
                             : loaded_id + 1U;
    }
    return Status::Ok();
}

Status InMemoryCatalog::RemoveIndex(IndexId id) {
    const auto iterator = indexes_by_id_.find(id.value());
    if (iterator == indexes_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "index ID does not exist")
            .WithContext("index_id", ToString(id));
    }
    index_ids_by_name_.erase(iterator->second.name);
    indexes_by_id_.erase(iterator);
    return Status::Ok();
}

Status InMemoryCatalog::UpdateIndexRoot(IndexId id, PageId root_page_id) {
    if (!root_page_id.valid()) {
        return Status::Error(ErrorCode::kInvalidArgument, "index root page ID is invalid");
    }
    const auto iterator = indexes_by_id_.find(id.value());
    if (iterator == indexes_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "index ID does not exist")
            .WithContext("index_id", ToString(id));
    }
    iterator->second.root_page_id = root_page_id;
    return Status::Ok();
}

Result<const InMemoryIndex*> InMemoryCatalog::FindIndexById(IndexId id) const {
    const auto iterator = indexes_by_id_.find(id.value());
    if (iterator == indexes_by_id_.end()) {
        return Status::Error(ErrorCode::kNotFound, "index ID does not exist")
            .WithContext("index_id", ToString(id));
    }
    return &iterator->second;
}

Result<const InMemoryIndex*> InMemoryCatalog::FindIndexForTableColumn(
    TableId table_id,
    std::size_t column_index) const {
    for (const auto& [unused_id, index] : indexes_by_id_) {
        static_cast<void>(unused_id);
        if (index.table_id == table_id && index.column_index == column_index) {
            return &index;
        }
    }
    return Status::Error(ErrorCode::kNotFound, "no index exists for the table column")
        .WithContext("table_id", ToString(table_id));
}

TableId InMemoryCatalog::NextTableId() {
    const TableId table_id{next_table_id_};
    ++next_table_id_;
    return table_id;
}

IndexId InMemoryCatalog::NextIndexId() {
    const IndexId index_id{next_index_id_};
    ++next_index_id_;
    return index_id;
}

}  // namespace kerndb
