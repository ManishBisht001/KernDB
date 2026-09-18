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

TableId InMemoryCatalog::NextTableId() {
    const TableId table_id{next_table_id_};
    ++next_table_id_;
    return table_id;
}

}  // namespace kerndb
